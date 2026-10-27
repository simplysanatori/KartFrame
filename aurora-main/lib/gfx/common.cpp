#include "common.hpp"
#include "../gx/geometry_cache.hpp"
#include "../gx/shader_info.hpp"
#include "geometry_buffer.hpp"

#include "clear.hpp"
#include "depth_peek.hpp"
#include "efb_ram_copy.hpp"
#include "eye_pass_plan.hpp"
#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "../gx/pipeline.hpp"
#include "pipeline_cache.hpp"
#include "hitch_stats.hpp"
#include "stereo_replay.hpp"
#include "anchored_interpolation.hpp"
#include "cockpit.hpp"
#include "window_mask.hpp"
#include "tex_copy_conv.hpp"
#include "tex_palette_conv.hpp"
#include "texture_replacement.hpp"
#include "texture.hpp"
#include "../window.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>

#include <absl/container/flat_hash_map.h>
#include <magic_enum.hpp>

#include "tracy/Tracy.hpp"

namespace aurora::gfx {
static Module Log("aurora::gfx");

using webgpu::g_device;
using webgpu::g_instance;
using webgpu::g_queue;

#ifdef AURORA_GFX_DEBUG_GROUPS
std::vector<std::string> g_debugGroupStack;
std::vector<std::string> g_debugMarkers;
#endif

constexpr uint64_t StagingBufferSize = UniformBufferSize + VertexBufferSize + IndexBufferSize + StorageBufferSize +
                                       (UseTextureBuffer ? TextureUploadSize : 0);

struct ShaderDrawCommand {
  ShaderType type;
  union {
    clear::DrawData clear;
    gx::DrawData gx;
  };
};
enum class CommandType {
  SetViewport,
  SetScissor,
  Draw,
  DebugMarker,
};
struct Command {
  CommandType type;
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> debugGroupStack;
#endif
  union Data {
    Viewport setViewport;
    ClipRect setScissor;
    ShaderDrawCommand draw;
    size_t debugMarkerIndex;
  } data;
};
} // namespace aurora::gfx

void aurora_set_guest_write_hooks(AuroraGuestWriteGenerationCallback generation,
                                  AuroraGuestWriteNotifyCallback notify) {
  aurora::g_guestWriteGenerationHook = generation;
  aurora::g_guestWriteNotifyHook = notify;
}

namespace aurora {
// For types that we can't ensure are safe to hash with has_unique_object_representations,
// we create specialized methods to handle them. Note that these are highly dependent on
// the structure definition, which could easily change with Dawn updates.
template <>
inline HashType xxh3_hash(const WGPUBindGroupDescriptor& input, HashType seed) {
  constexpr auto offset = offsetof(WGPUBindGroupDescriptor, layout); // skip nextInChain, label
  const auto hash = xxh3_hash_s(reinterpret_cast<const u8*>(&input) + offset,
                                sizeof(WGPUBindGroupDescriptor) - offset - sizeof(void*) /* skip entries */, seed);
  const size_t entryBytes = sizeof(WGPUBindGroupEntry) * input.entryCount;
  // The entries are hashed unseeded so they stay off XXH3's seeded long path;
  // the descriptor head still carries the caller's seed.
  return hash_combine(hash, xxh3_hash_s(input.entries, entryBytes));
}
template <>
inline HashType xxh3_hash(const wgpu::SamplerDescriptor& input, HashType seed) {
  constexpr auto offset = offsetof(wgpu::SamplerDescriptor, addressModeU); // skip nextInChain, label
  return xxh3_hash_s(reinterpret_cast<const u8*>(&input) + offset,
                     sizeof(wgpu::SamplerDescriptor) - offset - 2 /* skip padding */, seed);
}
} // namespace aurora

namespace aurora::gfx {
namespace {
struct CachedBindGroup {
  wgpu::BindGroup bindGroup;
  uint32_t lastUsedFrame = 0;
};

constexpr uint32_t BindGroupCacheRetainFrames = 32;
constexpr uint32_t BindGroupCacheSweepPeriod = 16;
} // namespace

static absl::flat_hash_map<BindGroupRef, CachedBindGroup> g_cachedBindGroups;
// Bind groups dropped from the cache by clear_caches() while a sealed frame's recorded draws
// still reference them. Ownership moves here and is released at the next seal.
static std::vector<CachedBindGroup> g_retiredBindGroups;
static absl::flat_hash_map<SamplerRef, wgpu::Sampler> g_cachedSamplers;

static ByteBuffer g_verts;
static ByteBuffer g_uniforms;
// Interpolation reads uniforms again while matching and retaining a scene.
// Keep those reads in cached CPU memory, not a write-combined upload heap.
// The destination is latched per batch so live setting changes cannot switch
// its backing storage before pending interpolation tasks have completed.
static std::vector<uint8_t> g_cpuUniforms;
static uint8_t* g_uniformUploadDestination = nullptr;
static ByteBuffer g_indices;
static ByteBuffer g_storage;
static ByteBuffer g_textureUpload;
wgpu::Buffer g_vertexBuffer;
wgpu::Buffer g_uniformBuffer;
wgpu::Buffer g_indexBuffer;
wgpu::Buffer g_storageBuffer;
constexpr size_t FrameSlotCount = 3;
static std::array<wgpu::Buffer, FrameSlotCount> g_stagingBuffers;
static size_t currentStagingBuffer = 0;
enum class BufferMapState {
  Unmapped,
  Mapping,
  Mapped,
};
static std::atomic s_mappingState{BufferMapState::Unmapped};
static wgpu::Limits g_cachedLimits;
// Advanced once per logical frame in the seal prologue, under the renderer GPU mutex and with the
// producer blocked, so every later reader sees a value that no longer moves.
static uint32_t g_frameIndex = UINT32_MAX;
wgpu::BindGroupLayout g_staticBindGroupLayout;
wgpu::BindGroup g_staticBindGroup;
wgpu::BindGroupLayout g_uniformBindGroupLayout;
wgpu::BindGroup g_uniformBindGroup;

// for imgui debug
AuroraStats g_stats{};
// GXState::deindexVertices and nativeVertexInputRequested, latched in begin_frame_impl (resident geometry, off by
// default; geometry_buffer.hpp).
static std::atomic_bool g_gxDeindexVertices{false};
static std::atomic_bool g_gxNativeVertexInput{false};
StagingStats g_stagingStats;
uint32_t g_drawCallCount = 0;
uint32_t g_mergedDrawCallCount = 0;

using CommandList = std::vector<Command>;
struct RenderPass {
  // The world depth mapping of this pass's last full-view perspective draw, for
  // the VR cockpit overlay (set by prepare_stereo_replay_uniforms).
  cockpit::SceneDepth cockpitDepth{};
  wgpu::TextureView colorView;
  wgpu::TextureView resolveView; // MSAA resolve target; null if msaaSamples == 1
  wgpu::TextureView depthView;
  wgpu::Texture copySourceTexture;
  wgpu::TextureView copySourceView;
  wgpu::TextureView copySourceDepthView;
  wgpu::Extent3D targetSize;
  uint32_t msaaSamples = 1;

  TextureHandle resolveTarget;
  TextureHandle resolveSourceSnapshot;
  GXTexFmt resolveFormat = GX_TF_RGBA8;
  ClipRect resolveRect;
  ClipRect resolveSnapshotRect;
  Vec4<float> resolveSourceRect;
  Range resolveUniformRange;
  std::array<u32, 3> resolveCopyFilterCoefficients{0, 64, 0};
  Vec4<float> clearColorValue{0.f, 0.f, 0.f, 0.f};
  // 1.f is the forward-Z "farthest" clear value; under UseReversedZ farthest is 0.f instead (see
  // gx::clear_depth_value(), which the main render pass explicitly overrides this default with -
  // any OTHER pass that keeps this default, e.g. an offscreen render-to-texture pass composited
  // later, needs the same reversed-Z-aware value or its depth buffer starts "already nearest",
  // failing every subsequent depth test and making whatever's drawn into it vanish).
  float clearDepthValue = gx::UseReversedZ ? 0.f : 1.f;
  CommandList commands;
  bool clearColor = true;
  bool clearDepth = true;
  // The resolve destination outlives the frame with no re-issue path (one-shot
  // bake), so this pass may not drop draws even in skip-unready-pipelines mode.
  bool requireReadyPipelines = false;
  bool resolveHalfScale = false;
  bool resolveCopyFilterActive = false;
  bool resolveForceOpaqueAlpha = false;
  bool resolveNeedsConversion = false;
  bool resolveNeedsShaderSampling = false;
  bool resolveLinearSampling = false;
  bool displayCopyResolve = false;
  // This pass is the continuation resolve_pass opened after a GXCopyDisp, so its
  // clears describe that copy's EFB reset rather than anything the game drew.
  // Deliberately not set for GXCopyTex: a mid-frame copy clear establishes the
  // background the rest of the frame draws over, and an eye still needs it.
  bool postCopyClear = false;
  bool snapshotColorResolveSource = false;
  bool efbTarget = false;
  std::vector<tex_palette_conv::ConvRequest> paletteConvs;
};
static std::vector<RenderPass> g_renderPasses;
static u32 g_currentRenderPass = UINT32_MAX;

// Immersive-replay EFB controls. The settings overlay writes these from the UI
// thread while the frame worker reads them mid-encode, so they are atomic. Both
// default to the corrected behaviour; clearing either restores the raw replay
// for A/B comparison without a rebuild.
static std::atomic_bool g_stereoStopAtDisplayCopy{true};
static std::atomic_bool g_stereoSkipCopyClears{true};
// Replays each eye in as few render passes as its clears allow (eye_pass_plan.hpp) rather than one
// per recorded pass. Same image, fewer tile loads and stores.
static std::atomic_bool g_stereoSinglePassEyes{true};
// foveation::Level of the immersive eyes.
static std::atomic_uint32_t g_stereoFoveation{0};

void set_stereo_stop_at_display_copy(bool value) noexcept {
  g_stereoStopAtDisplayCopy.store(value, std::memory_order_relaxed);
}
bool get_stereo_stop_at_display_copy() noexcept { return g_stereoStopAtDisplayCopy.load(std::memory_order_relaxed); }
void set_stereo_skip_copy_clears(bool value) noexcept {
  g_stereoSkipCopyClears.store(value, std::memory_order_relaxed);
}
bool get_stereo_skip_copy_clears() noexcept { return g_stereoSkipCopyClears.load(std::memory_order_relaxed); }
void set_stereo_single_pass_eyes(bool value) noexcept {
  g_stereoSinglePassEyes.store(value, std::memory_order_relaxed);
}
bool get_stereo_single_pass_eyes() noexcept { return g_stereoSinglePassEyes.load(std::memory_order_relaxed); }
void set_stereo_foveation(uint32_t level) noexcept {
  g_stereoFoveation.store(level, std::memory_order_relaxed);
}
uint32_t get_stereo_foveation() noexcept { return g_stereoFoveation.load(std::memory_order_relaxed); }

// The fixed virtual screen orthographic draws are placed on during immersive
// replay, in game world units. Written from the settings overlay and read by
// the frame worker, like the two controls above. A cleared enable, or a size or
// distance that is not positive, leaves 2D content on its recorded GX
// transforms, which stretches it across the whole eye.
static std::atomic_bool g_stereoHudScreenEnabled{false};
static std::atomic<float> g_stereoHudScreenWidth{0.f};
static std::atomic<float> g_stereoHudScreenDistance{0.f};
static std::atomic<float> g_stereoHudWorldDepth{0.f};
static std::atomic<float> g_stereoHudWorldAnchorX{0.f};
static std::atomic<float> g_stereoHudWorldAnchorY{0.f};

void set_stereo_hud_screen(bool enabled, float width, float distance) noexcept {
  g_stereoHudScreenWidth.store(width, std::memory_order_relaxed);
  g_stereoHudScreenDistance.store(distance, std::memory_order_relaxed);
  g_stereoHudScreenEnabled.store(enabled, std::memory_order_relaxed);
}
bool get_stereo_hud_screen_enabled() noexcept { return g_stereoHudScreenEnabled.load(std::memory_order_relaxed); }
void set_stereo_hud_world_depth(float distance) noexcept {
  g_stereoHudWorldDepth.store(std::isfinite(distance) && distance > 0.f ? distance : 0.f,
                              std::memory_order_relaxed);
}
float get_stereo_hud_world_depth() noexcept {
  return g_stereoHudWorldDepth.load(std::memory_order_relaxed);
}
void set_stereo_hud_world_anchor(float x, float y) noexcept {
  g_stereoHudWorldAnchorX.store(std::isfinite(x) ? x : 0.f, std::memory_order_relaxed);
  g_stereoHudWorldAnchorY.store(std::isfinite(y) ? y : 0.f, std::memory_order_relaxed);
}
std::pair<float, float> get_stereo_hud_world_anchor() noexcept {
  return {g_stereoHudWorldAnchorX.load(std::memory_order_relaxed),
          g_stereoHudWorldAnchorY.load(std::memory_order_relaxed)};
}
void get_stereo_hud_screen_size(float& width, float& distance) noexcept {
  width = g_stereoHudScreenWidth.load(std::memory_order_relaxed);
  distance = g_stereoHudScreenDistance.load(std::memory_order_relaxed);
}

// The desktop mirror choice. Normal is the ordinary mono presentation, so a
// build that never touches this setting presents exactly as it did before.
static std::atomic<AuroraStereoMirrorView> g_stereoMirrorView{AURORA_STEREO_MIRROR_NORMAL};

void set_stereo_mirror_view(AuroraStereoMirrorView value) noexcept {
  switch (value) {
  case AURORA_STEREO_MIRROR_NORMAL:
  case AURORA_STEREO_MIRROR_BOTH_EYES:
  case AURORA_STEREO_MIRROR_LEFT_EYE:
  case AURORA_STEREO_MIRROR_RIGHT_EYE:
  case AURORA_STEREO_MIRROR_NONE:
    break;
  default:
    // An out-of-range value would otherwise black the window out with no way
    // back from inside the game.
    Log.warn("Ignoring unknown stereo mirror view {}", static_cast<int>(value));
    return;
  }
  g_stereoMirrorView.store(value, std::memory_order_relaxed);
}
AuroraStereoMirrorView get_stereo_mirror_view() noexcept { return g_stereoMirrorView.load(std::memory_order_relaxed); }

// Recycle command storage: discarding passes used to free their command lists too, so each frame
// rebuilt hundreds of KB from zero capacity. The passes themselves are cheap to recreate.
using CommandListPool = std::vector<CommandList>;
static CommandListPool g_commandListPool;
static constexpr size_t MaxPooledCommandLists = 32;
// The pool is the only recording storage both sides touch, a few pointer moves per frame, so a
// plain mutex is cheaper than the alternatives and keeps the vector's invariants.
static std::mutex g_commandListPoolMutex;

static CommandList acquire_command_list() noexcept {
  std::lock_guard lock{g_commandListPoolMutex};
  if (g_commandListPool.empty()) {
    return {};
  }
  CommandList list = std::move(g_commandListPool.back());
  g_commandListPool.pop_back();
  list.clear();
  return list;
}

static void release_command_list(CommandList&& list) noexcept {
  if (list.capacity() == 0) {
    return;
  }
  std::lock_guard lock{g_commandListPoolMutex};
  if (g_commandListPool.size() >= MaxPooledCommandLists) {
    return;
  }
  list.clear();
  g_commandListPool.emplace_back(std::move(list));
}

static void release_render_pass(RenderPass& pass) noexcept { release_command_list(std::move(pass.commands)); }

static void recycle_render_passes(std::vector<RenderPass>& passes) noexcept {
  for (auto& pass : passes) {
    release_render_pass(pass);
  }
  passes.clear();
}

struct LateStereoUniform {
  gx::UniformReplayLayout layout;
  Viewport viewport;
  ClipRect displayRegion;
  ClipRect scissor;         // the draw's recorded scissor (2D screen clip)
  uint32_t clipOffset = 0;  // gx::screen_clip_offset of a 2D screen draw's uniform, 0: none
  Range current;
  Range previous;
  std::array<Range, AURORA_STEREO_EYE_COUNT> eyes;
};
struct LateStereoData {
  std::vector<LateStereoUniform> uniforms;
  std::vector<uint8_t> sources;
  std::vector<uint8_t> uploadBytes;
  ClipRect displayRegion{};
  stereo_replay::HudScreen hudScreen{};
  uint64_t generation = 0;
  uint32_t uploadOffset = 0;
  uint32_t uploadSize = 0;
};
// Advanced for every upload, including synchronous mid-frame EFB readbacks.
static std::atomic_uint64_t g_replayBufferGeneration{0};
static LateStereoData g_pendingLateStereo;
static thread_local EyeSceneRecord t_eyeScene;
EyeSceneRecord& eye_scene_record() noexcept { return t_eyeScene; }
// Lane P: the head pose an eye's uniform fill writes, at the fill itself (both fill sites call this right
// before write_stereo_uniform with the same eye transform).
static void note_eye_pose(uint32_t eye, const StereoReplayEye& view, const StereoReplayFrame& frame) noexcept {
  if (eye >= 2) return;
  auto& scene = t_eyeScene;
  if (!scene.posed[eye]) {
    scene.posed[eye] = true;
    scene.poseTime[eye] = frame.displayTimeNanos;
    scene.poseToken[eye] = frame.frameToken;
    scene.pose[eye] = view.viewFromCenter;
    scene.projection[eye] = view.projection;
    return;
  }
  if (scene.poseTime[eye] != frame.displayTimeNanos || scene.poseToken[eye] != frame.frameToken ||
      std::memcmp(&scene.pose[eye], &view.viewFromCenter, sizeof(Mat3x4<float>)) != 0 ||
      std::memcmp(&scene.projection[eye], &view.projection, sizeof(Mat4x4<float>)) != 0) {
    if (scene.poseMixed[eye] != UINT16_MAX) ++scene.poseMixed[eye];
  }
}
// The bytes the fill wrote for an eye's first range, at its offset in g_uniformBuffer (after write_stereo_uniform).
static void note_eye_probe(uint32_t eye, uint32_t offset, std::span<const uint8_t> written) noexcept {
  if (eye >= 2 || t_eyeScene.probed[eye]) return;
  auto& scene = t_eyeScene;
  const uint32_t size = static_cast<uint32_t>(std::min<size_t>(written.size(), scene.probeBytes[eye].size())) & ~3u;
  if (size == 0 || (offset & 3u) != 0) return;
  scene.probed[eye] = true;
  scene.probeOffset[eye] = offset;
  scene.probeSize[eye] = size;
  std::memcpy(scene.probeBytes[eye].data(), written.data(), size);
}
static void count_eye_scene(uint16_t& counter) noexcept {
  if (counter != UINT16_MAX) ++counter;
}
static uint32_t g_stereoLocalPlayerCount = 1;

void set_stereo_local_player_count(uint32_t count) noexcept {
  g_stereoLocalPlayerCount = count >= 1 && count <= 4 ? count : 1;
}

struct SealedFrameData {
  std::vector<RenderPass> passes;
  LateStereoData stereo;
  uint32_t localPlayerCount = 1;
  // The immersive window's screen as this frame's 2D layer was placed on it.
  stereo_replay::HudScreen windowScreen{};
};

SealedFrame::SealedFrame() : m_data(std::make_unique<SealedFrameData>()) {}
SealedFrame::~SealedFrame() = default;
SealedFrame::SealedFrame(SealedFrame&&) noexcept = default;
SealedFrame& SealedFrame::operator=(SealedFrame&&) noexcept = default;

static RenderPass& push_render_pass(RenderPass&& pass) {
  auto& out = g_renderPasses.emplace_back(std::move(pass));
  if (out.commands.capacity() == 0) {
    out.commands = acquire_command_list();
  }
  return out;
}
// The producer may build the next frame's GX stream while the worker prepares the target, and
// this is the only renderer-owned property that path queries, so publish it explicitly.
static std::atomic_bool g_inOffscreen{false};
static std::optional<RenderPass> g_suspendedEfbPass;
static Viewport g_suspendedEfbViewport;
static ClipRect g_suspendedEfbScissor;

static void discard_suspended_efb_pass() noexcept {
  if (g_suspendedEfbPass) {
    release_render_pass(*g_suspendedEfbPass);
    g_suspendedEfbPass.reset();
  }
}

static bool has_current_render_pass() noexcept { return g_currentRenderPass < g_renderPasses.size(); }
static webgpu::TextureWithSampler g_offscreenColor;
static webgpu::TextureWithSampler g_offscreenDepth;

struct ResolveSourceSnapshotPool {
  TextureHandle entry;
};

// Snapshot resources live in the same safe frame slots as the mapped staging buffers, and copies
// within a frame are ordered, so one grow-only snapshot serves every resolve in that slot.
static std::array<ResolveSourceSnapshotPool, FrameSlotCount> g_resolveSourceSnapshotPools;
static size_t g_recordingSnapshotSlot = 0;

static TextureHandle new_resolve_source_snapshot(wgpu::Extent3D size, wgpu::TextureFormat format) noexcept {
  const wgpu::TextureDescriptor textureDescriptor{
      .label = "GX Copy Source Snapshot",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = format,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);
  constexpr wgpu::TextureViewDescriptor viewDescriptor{
      .label = "GX Copy Source Snapshot view",
      .dimension = wgpu::TextureViewDimension::e2D,
  };
  auto textureView = texture.CreateView(&viewDescriptor);
  wgpu::TextureView sampleTextureView = textureView;
  return std::make_shared<TextureRef>(std::move(texture), std::move(sampleTextureView), std::move(textureView), size,
                                      format, 1, GX_TF_RGBA8);
}

static TextureHandle acquire_resolve_source_snapshot(uint32_t width, uint32_t height) noexcept {
  auto& pool = g_resolveSourceSnapshotPools[g_recordingSnapshotSlot];
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;

  if (pool.entry && pool.entry->size.width >= width && pool.entry->size.height >= height &&
      pool.entry->format == format) {
    return pool.entry;
  }

  const wgpu::Extent3D size{
      std::max(width, pool.entry ? pool.entry->size.width : 0u),
      std::max(height, pool.entry ? pool.entry->size.height : 0u),
      1,
  };
  pool.entry = new_resolve_source_snapshot(size, format);
  return pool.entry;
}

struct ResolveSamplingPlan {
  bool needsConversion = false;
  bool needsShaderSampling = false;
  bool usesLinearSampling = false;
};

static ResolveSamplingPlan make_resolve_sampling_plan(const TextureHandle& target, GXTexFmt format,
                                                      const ClipRect& resolveRect, const Vec4<float>& sourceRect,
                                                      bool halfScale, bool copyFilterActive,
                                                      bool forceOpaqueAlpha) noexcept {
  const auto differs = [](float lhs, float rhs) { return std::abs(lhs - rhs) > 0.01f; };
  const auto differsFromDst = [&](uint32_t dst, float src) { return differs(static_cast<float>(dst), src); };
  const bool sourceMatchesIntegerRect = !differs(sourceRect.x(), static_cast<float>(resolveRect.x)) &&
                                        !differs(sourceRect.y(), static_cast<float>(resolveRect.y)) &&
                                        !differs(sourceRect.z(), static_cast<float>(resolveRect.width)) &&
                                        !differs(sourceRect.w(), static_cast<float>(resolveRect.height));
  const bool dstMatchesSource = target && !differsFromDst(target->size.width, sourceRect.z()) &&
                                !differsFromDst(target->size.height, sourceRect.w());
  const bool needsShaderSampling =
      halfScale || copyFilterActive || forceOpaqueAlpha || !sourceMatchesIntegerRect || !dstMatchesSource;
  return {
      .needsConversion = tex_copy_conv::needs_conversion(format),
      .needsShaderSampling = needsShaderSampling,
      .usesLinearSampling = halfScale || (needsShaderSampling && !copyFilterActive),
  };
}

static ClipRect calculate_resolve_snapshot_rect(const wgpu::Extent3D& targetSize, const Vec4<float>& sourceRect,
                                                const ResolveSamplingPlan& samplingPlan,
                                                bool copyFilterActive) noexcept {
  const ClipRect fullTarget{
      .x = 0,
      .y = 0,
      .width = static_cast<int32_t>(targetSize.width),
      .height = static_cast<int32_t>(targetSize.height),
  };
  // Shader-sampled and converted copies can feed effects that read outside the copy rectangle
  // (MKW's DOF chain), so keep those full-target. Exact copies have a closed, crop-safe footprint.
  if (samplingPlan.needsConversion || samplingPlan.needsShaderSampling || targetSize.width == 0 ||
      targetSize.height == 0 || !std::isfinite(sourceRect.x()) || !std::isfinite(sourceRect.y()) ||
      !std::isfinite(sourceRect.z()) || !std::isfinite(sourceRect.w()) || sourceRect.z() <= 0.0f ||
      sourceRect.w() <= 0.0f) {
    return fullTarget;
  }

  // Linear sampling can touch one neighbor on every edge. The vertical GX
  // copy filter explicitly samples the previous and next rows.
  const int32_t haloX = samplingPlan.usesLinearSampling ? 1 : 0;
  const int32_t haloY = (samplingPlan.usesLinearSampling || copyFilterActive) ? 1 : 0;
  const int32_t targetWidth = static_cast<int32_t>(targetSize.width);
  const int32_t targetHeight = static_cast<int32_t>(targetSize.height);
  const int32_t left = std::clamp(static_cast<int32_t>(std::floor(sourceRect.x())) - haloX, 0, targetWidth - 1);
  const int32_t top = std::clamp(static_cast<int32_t>(std::floor(sourceRect.y())) - haloY, 0, targetHeight - 1);
  const int32_t right =
      std::clamp(static_cast<int32_t>(std::ceil(sourceRect.x() + sourceRect.z())) + haloX, left + 1, targetWidth);
  const int32_t bottom =
      std::clamp(static_cast<int32_t>(std::ceil(sourceRect.y() + sourceRect.w())) + haloY, top + 1, targetHeight);
  return {
      .x = left,
      .y = top,
      .width = right - left,
      .height = bottom - top,
  };
}

static void set_efb_targets(RenderPass& pass) {
  pass.colorView = webgpu::g_frameBuffer.view;
  pass.resolveView = webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_frameBufferResolved.view : nullptr;
  pass.depthView = webgpu::g_depthBuffer.view;
  pass.copySourceTexture =
      webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_frameBufferResolved.texture : webgpu::g_frameBuffer.texture;
  pass.copySourceView =
      webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_frameBufferResolved.view : webgpu::g_frameBuffer.view;
  pass.copySourceDepthView = webgpu::g_depthBuffer.view;
  pass.targetSize = webgpu::g_frameBuffer.size;
  pass.msaaSamples = webgpu::g_graphicsConfig.msaaSamples;
  pass.efbTarget = true;
}

struct OffscreenCacheKey {
  uint32_t width;
  uint32_t height;

  bool operator==(const OffscreenCacheKey& rhs) const { return width == rhs.width && height == rhs.height; }
  template <typename H>
  friend H AbslHashValue(H h, const OffscreenCacheKey& key) {
    return H::combine(std::move(h), key.width, key.height);
  }
};
struct OffscreenCacheEntry {
  webgpu::TextureWithSampler color;
  webgpu::TextureWithSampler depth;
};
static absl::flat_hash_map<OffscreenCacheKey, OffscreenCacheEntry> g_offscreenCache;
std::vector<TextureUpload> g_textureUploads;

static inline void push_command(CommandType type, const Command::Data& data) {
  if (!has_current_render_pass())
    UNLIKELY {
      Log.warn("Dropping command {}", magic_enum::enum_name(type));
      return;
    }
  g_renderPasses[g_currentRenderPass].commands.push_back({
      .type = type,
#ifdef AURORA_GFX_DEBUG_GROUPS
      .debugGroupStack = g_debugGroupStack,
#endif
      .data = data,
  });
}

template <>
gx::DrawData* get_last_draw_command() {
  if (g_currentRenderPass >= g_renderPasses.size()) {
    return nullptr;
  }
  auto& last = g_renderPasses[g_currentRenderPass].commands.back();
  if (last.type != CommandType::Draw || last.data.draw.type != ShaderType::GX) {
    return nullptr;
  }
  return &last.data.draw.gx;
}

static void push_draw_command(ShaderDrawCommand data) {
  push_command(CommandType::Draw, Command::Data{.draw = data});
  ++g_drawCallCount;
}

Vec2<uint32_t> get_frame_buffer_size() noexcept {
  if (webgpu::g_frameBuffer.size.width != 0 && webgpu::g_frameBuffer.size.height != 0) {
    return {webgpu::g_frameBuffer.size.width, webgpu::g_frameBuffer.size.height};
  }
  const auto windowSize = window::get_window_size();
  return {windowSize.fb_width, windowSize.fb_height};
}

// Render-thread only: the frame worker recycles g_renderPasses in end_frame, so an off-thread
// read is a use-after-free rather than a stale value. Use get_frame_buffer_size() instead.
Vec2<uint32_t> get_render_target_size() noexcept {
  if (g_currentRenderPass < g_renderPasses.size()) {
    const auto& size = g_renderPasses[g_currentRenderPass].targetSize;
    return {size.width, size.height};
  }
  return get_frame_buffer_size();
}

static Viewport g_cachedViewport;
void set_viewport(const Viewport& cmd) noexcept {
  if (cmd != g_cachedViewport) {
    push_command(CommandType::SetViewport, Command::Data{.setViewport = cmd});
    g_cachedViewport = cmd;
  }
}

static ClipRect g_cachedScissor;
void set_scissor(const ClipRect& cmd) noexcept {
  if (cmd != g_cachedScissor) {
    push_command(CommandType::SetScissor, Command::Data{.setScissor = cmd});
    g_cachedScissor = cmd;
  }
}

template <>
void push_draw_command(clear::DrawData data) {
  if (data.uniformRange.size == 0) {
    const std::array clearUniform{
        std::clamp(data.depth, 0.f, 1.f),
        0.f,
        0.f,
        0.f,
    };
    data.uniformRange = push_uniform(clearUniform);
  }
  push_draw_command(ShaderDrawCommand{.type = ShaderType::Clear, .clear = data});
}

template <>
PipelineRef pipeline_ref(const clear::PipelineConfig& config) {
  return find_pipeline(ShaderType::Clear, config, [=] { return create_pipeline(config); });
}

void resolve_pass(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                  Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat,
                  const Vec4<float>* sourceRectPixels, bool halfScale, const std::array<u32, 3>* copyFilterCoefficients,
                  bool forceOpaqueAlpha, float copyFilterRowStride, bool clampTop, bool clampBottom,
                  bool persistentCopy) {
  // Resolve current render pass
  if (!has_current_render_pass()) {
    Log.warn("Dropping resolve pass without an active render pass");
    return;
  }
  auto& prevPass = g_renderPasses[g_currentRenderPass];
  const auto targetWidth = static_cast<int32_t>(prevPass.targetSize.width);
  const auto targetHeight = static_cast<int32_t>(prevPass.targetSize.height);
  if (targetWidth <= 0 || targetHeight <= 0) {
    Log.warn("Dropping resolve pass with invalid target size {}x{}", targetWidth, targetHeight);
    return;
  }
  Vec4<float> sourceRect = sourceRectPixels != nullptr
                               ? *sourceRectPixels
                               : Vec4<float>{static_cast<float>(rect.x), static_cast<float>(rect.y),
                                             static_cast<float>(rect.width), static_cast<float>(rect.height)};
  if (targetWidth > 0 && targetHeight > 0) {
    const int32_t left = std::clamp(rect.x, 0, targetWidth - 1);
    const int32_t top = std::clamp(rect.y, 0, targetHeight - 1);
    const int32_t right = std::clamp(rect.x + rect.width, left + 1, targetWidth);
    const int32_t bottom = std::clamp(rect.y + rect.height, top + 1, targetHeight);
    rect = {
        .x = left,
        .y = top,
        .width = right - left,
        .height = bottom - top,
    };

    const float srcW = static_cast<float>(targetWidth);
    const float srcH = static_cast<float>(targetHeight);
    const float srcLeft = std::clamp(sourceRect.x(), 0.0f, srcW);
    const float srcTop = std::clamp(sourceRect.y(), 0.0f, srcH);
    const float srcRight = std::clamp(sourceRect.x() + sourceRect.z(), srcLeft, srcW);
    const float srcBottom = std::clamp(sourceRect.y() + sourceRect.w(), srcTop, srcH);
    sourceRect = {srcLeft, srcTop, std::max(srcRight - srcLeft, 1.0f), std::max(srcBottom - srcTop, 1.0f)};
  }
  prevPass.resolveTarget = std::move(texture);
  prevPass.requireReadyPipelines = persistentCopy;
  prevPass.resolveRect = rect;
  prevPass.resolveSourceRect = sourceRect;
  prevPass.resolveFormat = resolveFormat;
  prevPass.resolveHalfScale = halfScale;
  prevPass.resolveForceOpaqueAlpha = forceOpaqueAlpha;
  prevPass.resolveCopyFilterCoefficients =
      copyFilterCoefficients != nullptr ? *copyFilterCoefficients : std::array<u32, 3>{0, 64, 0};
  prevPass.resolveCopyFilterActive = prevPass.resolveCopyFilterCoefficients[0] != 0 ||
                                     prevPass.resolveCopyFilterCoefficients[1] != 64 ||
                                     prevPass.resolveCopyFilterCoefficients[2] != 0;
  const auto samplingPlan = make_resolve_sampling_plan(prevPass.resolveTarget, resolveFormat, rect, sourceRect,
                                                       halfScale, prevPass.resolveCopyFilterActive, forceOpaqueAlpha);
  prevPass.resolveNeedsConversion = samplingPlan.needsConversion;
  prevPass.resolveNeedsShaderSampling = samplingPlan.needsShaderSampling;
  prevPass.resolveLinearSampling = samplingPlan.usesLinearSampling;
  prevPass.snapshotColorResolveSource = !gx::is_depth_format(resolveFormat) && (clearColor || clearAlpha || clearDepth);
  Vec4<float> uniformSourceRect = sourceRect;
  float srcW = static_cast<float>(prevPass.targetSize.width);
  float srcH = static_cast<float>(prevPass.targetSize.height);
  if (prevPass.snapshotColorResolveSource) {
    prevPass.resolveSnapshotRect = calculate_resolve_snapshot_rect(prevPass.targetSize, sourceRect, samplingPlan,
                                                                   prevPass.resolveCopyFilterActive);
    prevPass.resolveSourceSnapshot =
        acquire_resolve_source_snapshot(static_cast<uint32_t>(prevPass.resolveSnapshotRect.width),
                                        static_cast<uint32_t>(prevPass.resolveSnapshotRect.height));
    uniformSourceRect = {sourceRect.x() - static_cast<float>(prevPass.resolveSnapshotRect.x),
                         sourceRect.y() - static_cast<float>(prevPass.resolveSnapshotRect.y), sourceRect.z(),
                         sourceRect.w()};
    srcW = static_cast<float>(prevPass.resolveSourceSnapshot->size.width);
    srcH = static_cast<float>(prevPass.resolveSourceSnapshot->size.height);
  }
  // GX's copy-clamp bits pin every vertical filter tap to the first or last source texel; without
  // it a filtered copy at internal resolution samples an unrelated EFB row as a visible border.
  const float clampTopPixels = clampTop ? uniformSourceRect.y() : 0.0f;
  const float clampBottomPixels = clampBottom ? uniformSourceRect.y() + uniformSourceRect.w() : srcH;
  const float clampTopUv = (clampTopPixels + 0.5f) / srcH;
  const float clampBottomUv = (clampBottomPixels - 0.5f) / srcH;
  // Push UV transform uniform for tex_copy_conv (crop region in UV space)
  const std::array resolveUniform{
      uniformSourceRect.x() / srcW,
      uniformSourceRect.y() / srcH,
      uniformSourceRect.z() / srcW,
      uniformSourceRect.w() / srcH,
      static_cast<float>(prevPass.resolveCopyFilterCoefficients[0]),
      static_cast<float>(prevPass.resolveCopyFilterCoefficients[1]),
      static_cast<float>(prevPass.resolveCopyFilterCoefficients[2]),
      prevPass.resolveCopyFilterActive ? 1.0f : 0.0f,
      prevPass.resolveForceOpaqueAlpha ? 1.0f : 0.0f,
      std::max(copyFilterRowStride, 1.0f),
      clampTopUv,
      clampBottomUv,
  };
  prevPass.resolveUniformRange = push_uniform(resolveUniform);
  const bool clearFullTarget = rect.x <= 0 && rect.y <= 0 &&
                               rect.width >= static_cast<int32_t>(prevPass.targetSize.width) &&
                               rect.height >= static_cast<int32_t>(prevPass.targetSize.height);
  const bool useAttachmentColorClear = clearFullTarget && clearColor && clearAlpha;
  const bool useAttachmentDepthClear = clearFullTarget && clearDepth;

  // Populate new render pass from previous
  const auto msaaSamples = prevPass.msaaSamples;
  RenderPass newPass{
      .colorView = prevPass.colorView,
      .resolveView = prevPass.resolveView,
      .depthView = prevPass.depthView,
      .copySourceTexture = prevPass.copySourceTexture,
      .copySourceView = prevPass.copySourceView,
      .copySourceDepthView = prevPass.copySourceDepthView,
      .targetSize = prevPass.targetSize,
      .msaaSamples = msaaSamples,
      .clearColorValue = clearColorValue,
      .clearDepthValue = clearDepthValue,
      .clearColor = useAttachmentColorClear,
      .clearDepth = useAttachmentDepthClear,
      // This continuation still renders into the same main EFB attachments.
      // Stereo replay filters on this flag; dropping it after a GX copy made
      // every later race pass mono-only and left the eye targets cleared.
      .efbTarget = prevPass.efbTarget,
  };
  push_render_pass(std::move(newPass));
  ++g_currentRenderPass;

  if ((!useAttachmentColorClear && (clearColor || clearAlpha)) || (!useAttachmentDepthClear && clearDepth)) {
    // GX copy clears cover the copied EFB rectangle, not always the whole target, so use a scissored
    // clear draw unless the load op can clear all of it.
    push_draw_command(clear::DrawData{
        .pipeline = pipeline_ref(clear::PipelineConfig{
            .msaaSamples = msaaSamples,
            .clearColor = clearColor,
            .clearAlpha = clearAlpha,
            .clearDepth = clearDepth,
        }),
        .stereoPipeline = aurora::stereo_frame_provider_active() ? pipeline_ref(clear::PipelineConfig{
            .msaaSamples = msaaSamples,
            .clearColor = clearColor,
            .clearAlpha = clearAlpha,
            .clearDepth = clearDepth,
            .stereoStencil = true,
        }) : 0,
        .color =
            wgpu::Color{
                .r = clearColorValue.x(),
                .g = clearColorValue.y(),
                .b = clearColorValue.z(),
                .a = clearColorValue.w(),
            },
        .depth = clearDepthValue,
        .useScissor = !clearFullTarget,
        .copyClear = true,
        .scissor = rect,
    });
  }
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

void mark_last_resolve_as_display_copy() noexcept {
  if (g_currentRenderPass == 0 || g_currentRenderPass > g_renderPasses.size()) {
    Log.warn("Could not identify the render pass preceding a GX display copy");
    return;
  }
  auto& resolvedPass = g_renderPasses[g_currentRenderPass - 1];
  if (!resolvedPass.resolveTarget || !resolvedPass.efbTarget) {
    Log.warn("GX display-copy marker did not follow a main-EFB resolve");
    return;
  }
  resolvedPass.displayCopyResolve = true;
  // resolve_pass has already opened the continuation that carries this copy's
  // EFB reset, and only here is it known to belong to a display copy. The guard
  // above admits g_currentRenderPass == size(), which has no continuation.
  if (g_currentRenderPass < g_renderPasses.size()) {
    g_renderPasses[g_currentRenderPass].postCopyClear = true;
  }
}

void queue_palette_conv(tex_palette_conv::ConvRequest req) {
  if (!has_current_render_pass()) {
    Log.warn("Dropping palette conversion without an active render pass");
    return;
  }
  g_renderPasses[g_currentRenderPass].paletteConvs.push_back(std::move(req));
}

bool is_offscreen() noexcept { return g_inOffscreen; }

uint32_t get_sample_count() noexcept {
  if (!has_current_render_pass()) {
    return webgpu::g_graphicsConfig.msaaSamples;
  }
  return g_renderPasses[g_currentRenderPass].msaaSamples;
}

void clear_caches() noexcept {
  g_offscreenCache.clear();
  // Retire rather than free; see g_retiredBindGroups.
  g_retiredBindGroups.reserve(g_retiredBindGroups.size() + g_cachedBindGroups.size());
  for (auto& entry : g_cachedBindGroups) {
    g_retiredBindGroups.emplace_back(std::move(entry.second));
  }
  g_cachedBindGroups.clear();
}

static OffscreenCacheEntry get_offscreen_textures(uint32_t width, uint32_t height) {
  OffscreenCacheKey key{width, height};
  if (const auto it = g_offscreenCache.find(key); it != g_offscreenCache.end()) {
    return it->second;
  }
  const auto colorFormat = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  const wgpu::Extent3D size{width, height, 1};
  const wgpu::TextureDescriptor colorDesc{
      .label = "Offscreen Color",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc |
               wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = colorFormat,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  auto colorTexture = g_device.CreateTexture(&colorDesc);
  auto colorView = colorTexture.CreateView();
  webgpu::TextureWithSampler color{
      .texture = std::move(colorTexture),
      .view = std::move(colorView),
      .size = size,
      .format = colorFormat,
  };
  const auto depthFormat = webgpu::g_graphicsConfig.depthFormat;
  const wgpu::TextureDescriptor depthDesc{
      .label = "Offscreen Depth",
      .usage = wgpu::TextureUsage::RenderAttachment,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = depthFormat,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  auto depthTexture = g_device.CreateTexture(&depthDesc);
  auto depthView = depthTexture.CreateView();
  webgpu::TextureWithSampler depth{
      .texture = std::move(depthTexture),
      .view = std::move(depthView),
      .size = size,
      .format = depthFormat,
  };
  OffscreenCacheEntry entry{
      .color = std::move(color),
      .depth = std::move(depth),
  };
  auto [insertIt, _] = g_offscreenCache.emplace(key, std::move(entry));
  return insertIt->second;
}

void begin_offscreen(uint32_t width, uint32_t height) {
  ZoneScoped;
  CHECK(g_currentRenderPass != UINT32_MAX, "begin_offscreen called outside of a frame");

  // If the current EFB pass has no resolve target, its output is unobservable.
  // Suspend it so that we can resume it after the offscreen pass.
  if (!g_inOffscreen) {
    auto& currentPass = g_renderPasses[g_currentRenderPass];
    if (!currentPass.resolveTarget) {
      g_suspendedEfbPass = std::move(currentPass);
      g_renderPasses.pop_back();
      --g_currentRenderPass;
    }
    g_suspendedEfbViewport = g_cachedViewport;
    g_suspendedEfbScissor = g_cachedScissor;
  }

  // Create offscreen textures
  auto offscreenEntry = get_offscreen_textures(width, height);
  g_offscreenColor = std::move(offscreenEntry.color);
  g_offscreenDepth = std::move(offscreenEntry.depth);

  // Start a new pass with offscreen targets
  RenderPass newPass{
      .colorView = g_offscreenColor.view,
      .depthView = g_offscreenDepth.view,
      .copySourceTexture = g_offscreenColor.texture,
      .copySourceView = g_offscreenColor.view,
      .copySourceDepthView = g_offscreenDepth.view,
      .targetSize = {width, height, 1},
      .msaaSamples = 1,
      .clearColorValue = {0.f, 0.f, 0.f, 0.f},
      // See the RenderPass::clearDepthValue default's comment: this offscreen pass gets its own
      // depth buffer, and the farthest clear value is 0.f, not 1.f, under UseReversedZ.
      .clearDepthValue = gx::UseReversedZ ? 0.f : 1.f,
      .clearColor = true,
      .clearDepth = true,
  };
  push_render_pass(std::move(newPass));
  ++g_currentRenderPass;

  g_inOffscreen = true;

  g_cachedViewport = {0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f};
  g_cachedScissor = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

void end_offscreen() {
  ZoneScoped;
  CHECK(g_inOffscreen, "end_offscreen called without begin_offscreen");

  g_inOffscreen = false;
  g_offscreenColor = {};
  g_offscreenDepth = {};

  // Resume suspended EFB pass, or start a new one (load existing content)
  if (g_suspendedEfbPass) {
    // Keeps its own recorded commands.
    g_renderPasses.emplace_back(std::move(*g_suspendedEfbPass));
    g_suspendedEfbPass.reset();
  } else {
    auto& pass = push_render_pass(RenderPass{});
    pass.clearColor = false;
    pass.clearDepth = false;
  }
  ++g_currentRenderPass;
  set_efb_targets(g_renderPasses[g_currentRenderPass]);

  g_cachedViewport = g_suspendedEfbViewport;
  g_cachedScissor = g_suspendedEfbScissor;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

template <>
void push_draw_command(gx::DrawData data) {
  push_draw_command(ShaderDrawCommand{.type = ShaderType::GX, .gx = data});
}

template <>
PipelineRef pipeline_ref(const gx::PipelineConfig& config) {
  return find_pipeline(ShaderType::GX, config, [=] { return create_pipeline(config); });
}

void initialize() {
  g_frameIndex = 0;
  depth_peek::initialize();
  tex_copy_conv::initialize();
  tex_palette_conv::initialize();
  texture_replacement::initialize();

  // For uniform & storage buffer offset alignments
  g_device.GetLimits(&g_cachedLimits);

  const auto createBuffer = [](wgpu::Buffer& out, wgpu::BufferUsage usage, uint64_t size, const char* label) {
    if (size <= 0) {
      return;
    }
    const wgpu::BufferDescriptor descriptor{
        .label = label,
        .usage = usage,
        .size = size,
    };
    out = g_device.CreateBuffer(&descriptor);
  };
  // CopySrc: lane P's uniform probe reads eye ranges back after a set's eyes (aurora.cpp UniformProbe).
  createBuffer(g_uniformBuffer, wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc,
               UniformBufferSize,
               "Shared Uniform Buffer");
  createBuffer(g_vertexBuffer, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, VertexBufferSize,
               "Shared Vertex Buffer");
  createBuffer(g_indexBuffer, wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst, IndexBufferSize,
               "Shared Index Buffer");
  createBuffer(g_storageBuffer, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, StorageBufferSize,
               "Shared Storage Buffer");
  for (int i = 0; i < g_stagingBuffers.size(); ++i) {
    const auto label = fmt::format("Staging Buffer {}", i);
    createBuffer(g_stagingBuffers[i], wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc, StagingBufferSize,
                 label.c_str());
  }
  currentStagingBuffer = 0;
  s_mappingState.store(BufferMapState::Unmapped, std::memory_order_release);
  map_staging_buffer();

  {
    constexpr std::array layoutEntries{
        // Vertex data buffer
        wgpu::BindGroupLayoutEntry{
            .binding = 0,
            .visibility = wgpu::ShaderStage::Vertex,
            .buffer =
                wgpu::BufferBindingLayout{
                    .type = wgpu::BufferBindingType::ReadOnlyStorage,
                },
        },
        // Storage data buffer
        wgpu::BindGroupLayoutEntry{
            .binding = 1,
            .visibility = wgpu::ShaderStage::Vertex,
            .buffer =
                wgpu::BufferBindingLayout{
                    .type = wgpu::BufferBindingType::ReadOnlyStorage,
                },
        },
    };
    const wgpu::BindGroupLayoutDescriptor layoutDesc{
        .label = "Static bind group layout",
        .entryCount = layoutEntries.size(),
        .entries = layoutEntries.data(),
    };
    g_staticBindGroupLayout = g_device.CreateBindGroupLayout(&layoutDesc);
    const std::array entries{
        wgpu::BindGroupEntry{
            .binding = 0,
            .buffer = g_vertexBuffer,
        },
        wgpu::BindGroupEntry{
            .binding = 1,
            .buffer = g_storageBuffer,
        },
    };
    const wgpu::BindGroupDescriptor bindGroupDescriptor{
        .label = "Static bind group",
        .layout = g_staticBindGroupLayout,
        .entryCount = entries.size(),
        .entries = entries.data(),
    };
    g_staticBindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
  }

  {
    constexpr std::array layoutEntries{
        // Uniform buffer (dynamic offset)
        wgpu::BindGroupLayoutEntry{
            .binding = 0,
            .visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
            .buffer =
                wgpu::BufferBindingLayout{
                    .type = wgpu::BufferBindingType::Uniform,
                    .hasDynamicOffset = true,
                },
        },
    };
    const wgpu::BindGroupLayoutDescriptor layoutDesc{
        .label = "Uniform bind group layout",
        .entryCount = layoutEntries.size(),
        .entries = layoutEntries.data(),
    };
    g_uniformBindGroupLayout = g_device.CreateBindGroupLayout(&layoutDesc);
    const std::array entries{
        wgpu::BindGroupEntry{
            .binding = 0,
            .buffer = g_uniformBuffer,
            .size = gx::MaxUniformSize,
        },
    };
    const wgpu::BindGroupDescriptor bindGroupDescriptor{
        .label = "Uniform bind group",
        .layout = g_uniformBindGroupLayout,
        .entryCount = entries.size(),
        .entries = entries.data(),
    };
    g_uniformBindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
  }

  gx::initialize();
  initialize_pipeline_cache();
}

void shutdown() {
  cockpit::shutdown();
  window_mask::shutdown();
  shutdown_pipeline_cache();
  gx::clear_shader_module_cache();
  efb_ram::shutdown();
  depth_peek::shutdown();
  tex_copy_conv::shutdown();
  tex_palette_conv::shutdown();
  texture_replacement::shutdown();
  gx::shutdown();

  g_uniformUploadDestination = nullptr;
  g_uniforms.release();
  std::vector<uint8_t>{}.swap(g_cpuUniforms);

  g_textureUploads.clear();
  g_cachedBindGroups.clear();
  g_retiredBindGroups.clear();
  g_cachedSamplers.clear();
  g_vertexBuffer = {};
  g_uniformBuffer = {};
  g_indexBuffer = {};
  g_storageBuffer = {};
  g_stagingBuffers.fill({});
  for (auto& pool : g_resolveSourceSnapshotPools) {
    pool.entry.reset();
  }
  discard_suspended_efb_pass();
  g_renderPasses.clear();
  g_commandListPool.clear();
  g_currentRenderPass = UINT32_MAX;
  g_offscreenCache.clear();
  g_offscreenColor = {};
  g_offscreenDepth = {};
  gx::geometry_cache::clear();
  detail::shutdown_geometry_buffer();
  g_staticBindGroup = {};
  g_staticBindGroupLayout = {};
  g_uniformBindGroup = {};
  g_uniformBindGroupLayout = {};
  g_inOffscreen = false;
  g_frameIndex = UINT32_MAX;
  currentStagingBuffer = 0;
  s_mappingState.store(BufferMapState::Unmapped, std::memory_order_release);
}

void map_staging_buffer() {
  auto expected = BufferMapState::Unmapped;
  if (!s_mappingState.compare_exchange_strong(expected, BufferMapState::Mapping, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
    return;
  }

  g_stagingBuffers[currentStagingBuffer].MapAsync(
      wgpu::MapMode::Write, 0, StagingBufferSize, wgpu::CallbackMode::AllowSpontaneous,
      [](wgpu::MapAsyncStatus status, wgpu::StringView message) {
        if (status == wgpu::MapAsyncStatus::CallbackCancelled || status == wgpu::MapAsyncStatus::Aborted) {
          Log.warn("Buffer mapping {}: {}", magic_enum::enum_name(status), message);
          s_mappingState.store(BufferMapState::Unmapped, std::memory_order_release);
          return;
        }
        ASSERT(status == wgpu::MapAsyncStatus::Success, "Buffer mapping failed: {} {}", magic_enum::enum_name(status),
               message);
        s_mappingState.store(BufferMapState::Mapped, std::memory_order_release);
      });
}

static bool begin_frame_impl(bool clearEfb) {
  ZoneScoped;
  {
    ZoneScopedN("Wait for buffer map");
    map_staging_buffer();
    while (true) {
      const auto mappingState = s_mappingState.load(std::memory_order_acquire);
      if (mappingState == BufferMapState::Mapped) {
        break;
      }
      if (mappingState == BufferMapState::Unmapped) {
        // Frame begin failed because the staging map was aborted; the caller's retry loop drops the frame
        // and any one-shot bakes recorded into it. Rate-limited.
        static uint32_t s_beginFrameMapFailCount = 0;
        if (s_beginFrameMapFailCount < 64 || (s_beginFrameMapFailCount & 255) == 0) {
          Log.warn("begin_frame aborted: staging buffer unmapped (frame={} occurrences={})", g_frameIndex,
                   s_beginFrameMapFailCount + 1);
        }
        ++s_beginFrameMapFailCount;
        return false;
      }
      g_instance.ProcessEvents();
    }
  }
  g_recordingSnapshotSlot = currentStagingBuffer;
  size_t bufferOffset = 0;
  const auto& stagingBuf = g_stagingBuffers[currentStagingBuffer];
  const auto mapBuffer = [&](ByteBuffer& buf, uint64_t size) {
    if (size <= 0) {
      return;
    }
    buf = ByteBuffer{static_cast<u8*>(stagingBuf.GetMappedRange(bufferOffset, size)), static_cast<size_t>(size)};
    bufferOffset += size;
  };
  mapBuffer(g_verts, VertexBufferSize);
  mapBuffer(g_uniforms, UniformBufferSize);
  g_uniformUploadDestination = nullptr;
  if (gx::stereo_frame_interpolation_active()) {
    g_cpuUniforms.resize(UniformBufferSize);
    g_uniformUploadDestination = g_uniforms.data();
    g_uniforms = ByteBuffer{g_cpuUniforms.data(), g_cpuUniforms.size()};
  }
  mapBuffer(g_indices, IndexBufferSize);
  mapBuffer(g_storage, StorageBufferSize);
  if constexpr (UseTextureBuffer) {
    mapBuffer(g_textureUpload, TextureUploadSize);
  }

  g_drawCallCount = 0;
  g_mergedDrawCallCount = 0;
  // Resident geometry (geometry_buffer.hpp). The resolved vertex layout is part of every shader config, so the switch
  // applies to whole frames and invalidates the pipeline-state memo like a register write would.
  if (const bool deindex = g_gxDeindexVertices.load(std::memory_order_relaxed);
      gx::g_gxState.deindexVertices != deindex) {
    gx::g_gxState.deindexVertices = deindex;
    gx::g_gxState.pipelineStateGeneration = gx::next_gx_state_epoch();
    gx::g_gxState.stateDirty = true;
  }
  // Per list, not per shader config: a cached list resolved either way keeps its own key, so no re-resolution.
  gx::g_gxState.nativeVertexInputRequested = g_gxNativeVertexInput.load(std::memory_order_relaxed);
  gx::geometry_cache::begin_frame(g_frameIndex);
  detail::log_resident_geometry(g_frameIndex);
  if (clearEfb) {
    gx::begin_frame_interpolation();
  }
  discard_suspended_efb_pass();
  webgpu::clear_present_source_override();

  push_render_pass(RenderPass{});
  set_efb_targets(g_renderPasses[0]);
  g_renderPasses[0].clearColorValue = gx::g_gxState.clearColor;
  g_renderPasses[0].clearDepthValue = gx::clear_depth_value();
  g_renderPasses[0].clearColor = clearEfb;
  g_renderPasses[0].clearDepth = clearEfb;
  g_currentRenderPass = 0;
  // Refresh paired render viewport/scissor from logical state in case the FB size changed.
  const auto mappedRenderState = gx::map_logical_render_state();
  const bool renderStateChanged = gx::g_gxState.renderViewport != mappedRenderState.viewport ||
                                  gx::g_gxState.renderScissor != mappedRenderState.scissor;
  gx::g_gxState.renderViewport = mappedRenderState.viewport;
  gx::g_gxState.renderScissor = mappedRenderState.scissor;
  gx::g_gxState.stateDirty = gx::g_gxState.stateDirty || renderStateChanged;
  g_cachedViewport = mappedRenderState.viewport;
  g_cachedScissor = mappedRenderState.scissor;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
  begin_pipeline_frame();
  return true;
}

bool begin_frame() { return begin_frame_impl(true); }

bool resume_frame() { return begin_frame_impl(false); }

void abort_frame() noexcept {
  efb_ram::cancel();
  efb_ram::abort_async();
  // Records queued for the geometry buffer never reach it: forget their entries too.
  detail::drop_geometry_uploads();
  gx::geometry_cache::uploads_dropped();
  g_verts.release();
  g_uniforms.release();
  g_uniformUploadDestination = nullptr;
  g_indices.release();
  g_storage.release();
  if constexpr (UseTextureBuffer) {
    g_textureUploads.clear();
    g_textureUpload.release();
  }
  if (s_mappingState.load(std::memory_order_acquire) == BufferMapState::Mapped) {
    // Pending interpolation tasks hold raw pointers into the mapped staging
    // range; they must be dropped before the buffer is unmapped and rotated.
    gx::drop_pending_frame_interpolation_uniforms();
    g_stagingBuffers[currentStagingBuffer].Unmap();
    s_mappingState.store(BufferMapState::Unmapped, std::memory_order_release);
    currentStagingBuffer = (currentStagingBuffer + 1) % g_stagingBuffers.size();
    map_staging_buffer();
  }
  recycle_render_passes(g_renderPasses);
  g_currentRenderPass = UINT32_MAX;
  discard_suspended_efb_pass();
  g_inOffscreen = false;
  for (auto& array : gx::g_gxState.arrays) {
    array.cachedRange = {};
  }
  webgpu::clear_present_source_override();
  end_pipeline_frame();
}

// What immersive replay should reproduce out of a sealed frame: the rectangle
// the game presented, and the last pass that still contributes to it.
struct StereoDisplaySource {
  ClipRect region{};
  // Inclusive index of the pass holding the final GXCopyDisp resolve. Passes
  // after it only reset the EFB for the next frame, so an eye that replays them
  // erases the image it just built. -1 means no display copy was found and the
  // whole pass list is replayed against the full-EFB fallback region.
  int32_t lastDisplayCopyPass = -1;
  bool foundDisplayCopy = false;
};

static StereoDisplaySource stereo_display_source(const std::vector<RenderPass>& passes) noexcept {
  StereoDisplaySource out{};
  ClipRect fullRegion{};
  for (const auto& pass : passes) {
    if (!pass.efbTarget || pass.targetSize.width == 0 || pass.targetSize.height == 0) {
      continue;
    }
    fullRegion = {
        .x = 0,
        .y = 0,
        .width = static_cast<int32_t>(pass.targetSize.width),
        .height = static_cast<int32_t>(pass.targetSize.height),
    };
    break;
  }

  // The desktop present source is replaced by each GXCopyDisp, so the final
  // valid display copy—not a union of every copy—is the frame shown to users.
  for (auto it = passes.rbegin(); it != passes.rend(); ++it) {
    const auto& pass = *it;
    if (!pass.efbTarget || !pass.displayCopyResolve || pass.targetSize.width == 0 || pass.targetSize.height == 0) {
      continue;
    }
    const int32_t passIndex = static_cast<int32_t>(std::distance(passes.begin(), it.base())) - 1;

    const int32_t targetWidth = static_cast<int32_t>(pass.targetSize.width);
    const int32_t targetHeight = static_cast<int32_t>(pass.targetSize.height);
    const int32_t left = std::clamp(pass.resolveRect.x, 0, targetWidth);
    const int32_t top = std::clamp(pass.resolveRect.y, 0, targetHeight);
    const int32_t right = std::clamp(pass.resolveRect.x + pass.resolveRect.width, left, targetWidth);
    const int32_t bottom = std::clamp(pass.resolveRect.y + pass.resolveRect.height, top, targetHeight);
    if (right <= left || bottom <= top) {
      continue;
    }
    out.region = {left, top, right - left, bottom - top};
    out.lastDisplayCopyPass = passIndex;
    out.foundDisplayCopy = true;
    return out;
  }

  out.region = fullRegion;
  return out;
}

// Logs each distinct region once (the last few remembered): the race alternates between two regions
// (the display copy and the full-EFB fallback) frame by frame, which logged 30 lines a second.
static void log_stereo_display_source_region(ClipRect region, bool foundDisplayCopy) noexcept {
  static std::array<std::pair<ClipRect, bool>, 8> seen{};
  static size_t seenCount = 0;
  // How often a frame had no display copy (the fallback region scales the 2D layer differently).
  static uint64_t found = 0, fallback = 0;
  static auto windowStart = std::chrono::steady_clock::now();
  ++(foundDisplayCopy ? found : fallback);
  if (const auto now = std::chrono::steady_clock::now(); now - windowStart >= std::chrono::seconds(10)) {
    if (fallback != 0) {
      Log.info("Immersive display-copy source: {} frames with a display copy, {} with the full-EFB fallback (10 s)",
               found, fallback);
    }
    found = fallback = 0;
    windowStart = now;
  }
  if (region.width <= 0 || region.height <= 0) {
    return;
  }
  const size_t known = std::min(seenCount, seen.size());
  for (size_t i = 0; i < known; ++i) {
    if (seen[i].first == region && seen[i].second == foundDisplayCopy) {
      return;
    }
  }
  seen[seenCount++ % seen.size()] = {region, foundDisplayCopy};
  {
    Log.info("Immersive display-copy source region: {}x{} at ({}, {}){}", region.width, region.height, region.x,
             region.y, foundDisplayCopy ? "" : " (full-EFB fallback)");
  }
}

// The virtual screen orthographic draws are placed on, sized from the aspect
// ratio the game is currently presenting at: 4:3 while VILockAspectRatio holds
// it there, otherwise the mirror window's own aspect, which is what Mario Kart
// Wii's dynamic widescreen builds its projections from. Matching it keeps the
// HUD unstretched on the screen. The immersive window is this same screen, and
// has it whether or not the 2D layer is set to go on it.
static stereo_replay::HudScreen stereo_window_screen() noexcept {
  const float width = g_stereoHudScreenWidth.load(std::memory_order_relaxed);
  const float distance = g_stereoHudScreenDistance.load(std::memory_order_relaxed);
  float aspect = 0.f;
  if (!window::get_present_aspect_ratio(aspect) || !(aspect > 0.f)) {
    aspect = 16.f / 9.f;
  }
  const float halfWidth = width * 0.5f;
  return {
      .halfWidth = halfWidth,
      .halfHeight = halfWidth / aspect,
      .distance = distance,
  };
}

static stereo_replay::HudScreen stereo_hud_screen() noexcept {
  if (!g_stereoHudScreenEnabled.load(std::memory_order_relaxed)) {
    return {};
  }
  return stereo_window_screen();
}

// Shared by the normal seal and headset-deadline replay. Head transforms are
// composed after scene interpolation, so free look never inherits its delay.
// The clip block of a virtual-screen 2D draw (gx::ScreenClipBytes at clipOffset): rows taking an eye
// pixel to homogeneous frame NDC on `screen` (as seen through `view`), and the frame NDC rectangle of
// the draw's recorded scissor within the frame. The fragment shader drops what falls
// outside, so the frame edge and the game's scissor clip as on a TV.
static void write_screen_clip(std::span<uint8_t> uniform, uint32_t clipOffset, const StereoReplayEye& eye,
                              const Mat3x4<float>& view, const stereo_replay::HudScreen& screen, bool clip,
                              const Viewport& drawViewport, ClipRect scissor, ClipRect displayRegion) noexcept {
  if (clipOffset + gx::ScreenClipBytes > uniform.size()) {
    return;
  }
  // KARTFRAME_HUD_CLIP=0: the old behaviour (no clip), for comparisons.
  static const bool clipEnabled = [] {
    const char* env = std::getenv("KARTFRAME_HUD_CLIP");
    return env == nullptr || env[0] != '0';
  }();
  clip = clip && clipEnabled;
  std::array<float, 16> block{1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, gx::ScreenClipMarker,
                              -1e30f, -1e30f, 1e30f, 1e30f};
  const auto mask = clip ? stereo_replay::window_mask(eye.projection, view, screen) : stereo_replay::WindowMask{};
  const float width = static_cast<float>(eye.target.size.width);
  const float height = static_cast<float>(eye.target.size.height);
  if (clip && (mask.w.x != 0.f || mask.w.y != 0.f || mask.w.z != 0.f) && width > 0.f && height > 0.f &&
      displayRegion.width > 0 && displayRegion.height > 0) {
    // NDC x = 2 px / W - 1, y = 1 - 2 py / H, folded into each row.
    const auto row = [&](const Vec3<float>& r, size_t at) {
      block[at + 0] = 2.f * r.x / width;
      block[at + 1] = -2.f * r.y / height;
      block[at + 2] = r.z - r.x + r.y;
    };
    row(mask.u, 0);
    row(mask.v, 4);
    row(mask.w, 8);
    const float regionLeft = static_cast<float>(displayRegion.x);
    const float regionTop = static_cast<float>(displayRegion.y);
    const float regionRight = regionLeft + static_cast<float>(displayRegion.width);
    const float regionBottom = regionTop + static_cast<float>(displayRegion.height);
    // The frame and the game's scissor, not the viewport: GX can draw outside its viewport (clipping
    // off), the scissor and the frame's own edges are what bound a draw on a TV.
    (void)drawViewport;
    float left = regionLeft;
    float top = regionTop;
    float right = regionRight;
    float bottom = regionBottom;
    if (scissor.width > 0 && scissor.height > 0) {
      left = std::max(left, static_cast<float>(scissor.x));
      top = std::max(top, static_cast<float>(scissor.y));
      right = std::min(right, static_cast<float>(scissor.x + scissor.width));
      bottom = std::min(bottom, static_cast<float>(scissor.y + scissor.height));
    }
    const float w = static_cast<float>(displayRegion.width);
    const float h = static_cast<float>(displayRegion.height);
    block[12] = 2.f * (left - regionLeft) / w - 1.f;
    block[13] = 1.f - 2.f * (bottom - regionTop) / h;
    block[14] = 2.f * (right - regionLeft) / w - 1.f;
    block[15] = 1.f - 2.f * (top - regionTop) / h;
  }
  std::memcpy(uniform.data() + clipOffset, block.data(), sizeof(block));
}

// Checks write_screen_clip's conventions against the projection the 2D draws actually get: frame points
// placed on the screen by compose_hud_screen_projection must come back from the clip rows unchanged.
// Logged with the 10 s screen line; an error above a few thousandths means the clip would cut the HUD.
static float screen_clip_self_check(const StereoReplayEye& eye, const stereo_replay::HudScreen& screen) noexcept {
  Mat4x4<float> identity{};
  identity.m0 = {1.f, 0.f, 0.f, 0.f};
  identity.m1 = {0.f, 1.f, 0.f, 0.f};
  identity.m2 = {0.f, 0.f, 1.f, 0.f};
  identity.m3 = {0.f, 0.f, 0.f, 1.f};
  const auto projection = stereo_replay::compose_hud_screen_projection(eye.projection, eye.viewFromCenter, screen, identity);
  std::array<uint8_t, gx::ScreenClipBytes> bytes{};
  const ClipRect region{0, 0, 640, 480};
  const Viewport full{0.f, 0.f, 640.f, 480.f, 0.f, 1.f};
  write_screen_clip({bytes.data(), bytes.size()}, 0, eye, eye.viewFromCenter, screen, true, full, {}, region);
  std::array<float, 16> block;
  std::memcpy(block.data(), bytes.data(), sizeof(block));
  const float width = static_cast<float>(eye.target.size.width);
  const float height = static_cast<float>(eye.target.size.height);
  float worst = 0.f;
  for (const auto& [u, v] : std::array<std::pair<float, float>, 4>{{{-0.6f, -0.4f}, {0.5f, 0.7f}, {-0.9f, 0.9f}, {0.8f, -0.8f}}}) {
    const auto row = [&](const Vec4<float>& r) { return r[0] * u + r[1] * v + r[3]; };
    const float w = row(projection.m3);
    if (!(w > 0.f)) {
      return -1.f;
    }
    const float px = (row(projection.m0) / w + 1.f) * 0.5f * width;
    const float py = (1.f - row(projection.m1) / w) * 0.5f * height;
    const float hu = block[0] * px + block[1] * py + block[2];
    const float hv = block[4] * px + block[5] * py + block[6];
    const float hw = block[8] * px + block[9] * py + block[10];
    if (!(hw > 0.f)) {
      return 1e9f;
    }
    worst = std::max({worst, std::fabs(hu / hw - u), std::fabs(hv / hw - v)});
  }
  return worst;
}

// The latest screen_clip_self_check, from the late replay (the only place with real eye poses); -2: none yet.
static std::atomic<float> g_screenClipCheck{-2.f};

static void write_stereo_uniform(std::span<uint8_t> uniform, const gx::UniformReplayLayout& layout,
                                 const StereoReplayEye& eye, const Mat4x4<float>& gameProjection,
                                 const Viewport& drawViewport, ClipRect displayRegion,
                                 const stereo_replay::HudScreen& hudScreen, bool positionsAnchored = false,
                                 ClipRect scissor = {}, uint32_t clipOffset = 0) noexcept {
  if (layout.perspective) {
    // A projection that flips X (mirror mode) keeps its flip: the eye frustum
    // replaces the X scale's magnitude, and the reflection moves onto the eye
    // transform's half of the composition, where it reverses the winding the
    // draw's own cull mode already expects.
    const bool mirrored = stereo_replay::projection_mirrors_x(gameProjection);
    auto projection = stereo_replay::compose_projection(eye.projection, gameProjection);
    if (mirrored) {
      projection = stereo_replay::mirror_projection_x(projection);
    }
    const auto viewFromScene = positionsAnchored
        ? (mirrored ? stereo_replay::mirror_view_delta_x(eye.viewFromCenter) : eye.viewFromCenter)
        : (mirrored ? eye.viewFromSceneMirrored : eye.viewFromScene);
    std::memcpy(uniform.data() + layout.projectionOffset, &projection, sizeof(projection));

    for (uint32_t matrix = 0; matrix < layout.positionMatrixCount; ++matrix) {
      if ((layout.positionMatrixMask & (1u << matrix)) == 0) {
        continue;
      }
      const size_t offset = layout.positionOffset + matrix * sizeof(Mat3x4<float>);
      Mat3x4<float> source;
      std::memcpy(&source, uniform.data() + offset, sizeof(source));
      const auto transformed = stereo_replay::compose_affine(viewFromScene, source);
      std::memcpy(uniform.data() + offset, &transformed, sizeof(transformed));
    }
    for (uint32_t matrix = 0; matrix < layout.normalMatrixCount; ++matrix) {
      const size_t offset = layout.normalOffset + matrix * sizeof(Mat3x4<float>);
      Mat3x4<float> source;
      std::memcpy(&source, uniform.data() + offset, sizeof(source));
      const auto transformed = stereo_replay::compose_normal(viewFromScene, source);
      std::memcpy(uniform.data() + offset, &transformed, sizeof(transformed));
    }
  } else {
    // 2D content reaches the eye entirely through its projection: the
    // draw's own position matrices lay the element out in screen space.
    // The screen rectangle is built in the VR-neutral view space, so this
    // path uses viewFromCenter, not viewFromScene: folding the anchor in
    // would leave the screen behind at the camera the anchor replaced.
    // First lift viewport-local NDC into displayed-frame NDC; replay will
    // use a full-eye viewport so sub-pane elements are not transformed by
    // the recorded viewport a second time.
    const auto ndcRemap = stereo_replay::make_hud_ndc_remap(
        drawViewport.left, drawViewport.top, drawViewport.width, drawViewport.height,
        static_cast<float>(displayRegion.x), static_cast<float>(displayRegion.y),
        static_cast<float>(displayRegion.width), static_cast<float>(displayRegion.height));
    auto screen = hudScreen;
    const bool worldNameBalloon = layout.worldHudDepth > 0.f && screen.valid();
    if (worldNameBalloon) {
      const float depth = std::clamp(layout.worldHudDepth, screen.distance * 0.5f,
                                     screen.distance * 20.f);
      const float scale = depth / screen.distance;
      screen.halfWidth *= scale;
      screen.halfHeight *= scale;
      screen.distance = depth;
    }
    const stereo_replay::HudOverlayPlacement placement = worldNameBalloon
        ? stereo_replay::HudOverlayPlacement{layout.worldHudAnchorX, layout.worldHudAnchorY, 0.55f, 0.07f}
        : stereo_replay::HudOverlayPlacement{};
    if (eye.handHud && !worldNameBalloon) {
      screen.halfWidth = .15f;
      screen.halfHeight = .15f * hudScreen.halfHeight / hudScreen.halfWidth;
      screen.distance = 1.0f;
    }
    const auto& view =
        worldNameBalloon ? eye.viewFromScene : eye.handHud ? eye.handHudViewFromPanel : eye.viewFromCenter;
    const auto projection =
        stereo_replay::compose_hud_screen_projection(eye.projection, view, screen, gameProjection, ndcRemap, placement);
    std::memcpy(uniform.data() + layout.projectionOffset, &projection, sizeof(projection));
    // Name balloons move with their kart's world depth and anchor: left unclipped.
    if (clipOffset != 0) {
      write_screen_clip(uniform, clipOffset, eye, view, screen, !worldNameBalloon, drawViewport, scissor, displayRegion);
    }
  }

  if (displayRegion.width > 0 && displayRegion.height > 0) {
    float renderSize[2];
    float logicalSize[2];
    std::memcpy(renderSize, uniform.data() + 8, sizeof(renderSize));
    std::memcpy(logicalSize, uniform.data() + 16, sizeof(logicalSize));
    if (layout.perspective) {
      renderSize[0] *= static_cast<float>(eye.target.size.width) / static_cast<float>(displayRegion.width);
      renderSize[1] *= static_cast<float>(eye.target.size.height) / static_cast<float>(displayRegion.height);
    } else {
      // Point/line expansion and GX's pixel-center correction now operate
      // in the full eye viewport. Recover the complete logical frame size
      // from this draw's logical-to-render scale.
      if (renderSize[0] != 0.0f) {
        logicalSize[0] *= static_cast<float>(displayRegion.width) / renderSize[0];
      }
      if (renderSize[1] != 0.0f) {
        logicalSize[1] *= static_cast<float>(displayRegion.height) / renderSize[1];
      }
      renderSize[0] = static_cast<float>(eye.target.size.width);
      renderSize[1] = static_cast<float>(eye.target.size.height);
      std::memcpy(uniform.data() + 16, logicalSize, sizeof(logicalSize));
    }
    std::memcpy(uniform.data() + 8, renderSize, sizeof(renderSize));
  }
}

// KARTFRAME_LOG_2D_QUADS=1 (diagnostics): the distinct 2D quads of the immersive replay, once each, with
// whether they went across the eye (full view) or onto the virtual screen.
static void log_replay_quad(const gx::DrawData& draw, bool fullView) noexcept {
  // =1: full-view draws and quads sampling an EFB copy (menus draw thousands of others); =x<n>: quads with
  // an edge within 4 px of frame x = n.
  static const float edgeX = [] {
    const char* env = std::getenv("KARTFRAME_LOG_2D_QUADS");
    return env != nullptr && env[0] == 'x' ? static_cast<float>(std::atof(env + 1)) : -1.f;
  }();
  static const bool enabled = [] {
    const char* env = std::getenv("KARTFRAME_LOG_2D_QUADS");
    return env != nullptr && (env[0] == '1' || env[0] == 'x');
  }();
  if (!enabled || !draw.screenRect) {
    return;
  }
  if (edgeX >= 0.f) {
    const auto& q = *draw.screenRect;
    if (std::fabs(q.left - edgeX) > 4.f && std::fabs(q.left + q.width - edgeX) > 4.f) {
      return;
    }
  } else if (!fullView && ((draw.quadSignature >> 32) & 1) == 0) {
    return;
  }
  static std::vector<std::pair<uint64_t, uint64_t>> seen;
  const auto& r = *draw.screenRect;
  const uint64_t rect = uint64_t(int(r.left) + 4096) | (uint64_t(int(r.top) + 4096) << 13) | (uint64_t(int(r.width)) << 26) |
                        (uint64_t(int(r.height)) << 39) | (uint64_t(fullView) << 52);
  const std::pair<uint64_t, uint64_t> key{rect, draw.quadSignature};
  if (seen.size() >= 600 || std::find(seen.begin(), seen.end(), key) != seen.end()) {
    return;
  }
  seen.push_back(key);
  const uint64_t s = draw.quadSignature;
  Log.info("Immersive 2D quad{}: {:.1f}x{:.1f} at ({:.1f}, {:.1f}); blend {} logic {} texgens {} tex0 {}x{}{}",
           fullView ? " (full view)" : "", r.width, r.height, r.left, r.top, s & 3, (s >> 2) & 15, (s >> 6) & 15,
           (s >> 10) & 2047, (s >> 21) & 2047, ((s >> 32) & 1) != 0 ? " (EFB copy)" : "");
}

// Each class of full-frame 2D draw in an immersive frame, once, with where it goes: across the whole
// eye (an untextured constant-colour fade) or not (a textured composite keeps the virtual screen or,
// sampling a fresh EFB copy, the framebuffer-effect path).
static void log_full_frame_class(const gx::DrawData& draw, bool fullView, const ClipRect& display) noexcept {
  if (!draw.screenRect || draw.uniformReplayLayout.perspective || display.width <= 0 || display.height <= 0) {
    return;
  }
  const auto& r = *draw.screenRect;
  const float w = static_cast<float>(display.width), h = static_cast<float>(display.height);
  if (!(r.left <= display.x + 0.02f * w && r.top <= display.y + 0.02f * h && r.left + r.width >= display.x + 0.98f * w &&
        r.top + r.height >= display.y + 0.98f * h)) {
    return;
  }
  static std::mutex mutex;
  static std::vector<uint64_t> seen;
  const uint64_t s = draw.quadSignature;
  const uint64_t key = (s & 0x1'0000'03ffull) | (uint64_t(fullView) << 40) | (uint64_t(draw.fadeCandidate) << 41) |
                       (uint64_t(draw.uniformReplayLayout.nativeEfbEffect) << 42);
  std::lock_guard lock(mutex);
  if (seen.size() >= 48 || std::ranges::find(seen, key) != seen.end()) {
    return;
  }
  seen.push_back(key);
  const uint32_t texgens = (s >> 6) & 15;
  Log.info("Full-frame 2D draw class: blend {} logic {} texgens {}{}{} -> {}", s & 3, (s >> 2) & 15, texgens,
           ((s >> 32) & 1) != 0 ? " (EFB copy)" : "", draw.invisible ? " (transparent)" : "",
           fullView ? "across the whole eye (fade)"
           : draw.uniformReplayLayout.nativeEfbEffect ? "framebuffer-effect path (recorded transforms, each eye)"
           : texgens != 0 ? "virtual screen (textured composite, not a fade)"
                          : "virtual screen");
}

static bool prepare_stereo_replay_uniforms(const StereoReplayFrame& stereoFrame,
                                           LateStereoData* history = nullptr) noexcept {
  const StereoDisplaySource displaySource = stereo_display_source(g_renderPasses);
  const ClipRect displayRegion = displaySource.region;
  const bool multiplayer = g_stereoLocalPlayerCount > 1;
  const auto playerRegion = stereo_replay::player_one_region(
      {float(displayRegion.x), float(displayRegion.y), float(displayRegion.width), float(displayRegion.height)},
      g_stereoLocalPlayerCount);
  // This is the producer-side preparation path; eye replay can query the pure
  // helper concurrently without touching this diagnostic state.
  log_stereo_display_source_region(displayRegion, displaySource.foundDisplayCopy);
  // The immersive window always carries the 2D layer: stretched across the eye,
  // it would be cut by the window's edges.
  const stereo_replay::HudScreen hudScreen = stereoFrame.window ? stereo_window_screen() : stereo_hud_screen();
  // A draw is replayed per eye when it carries the game camera (perspective) or
  // when it is 2D content the virtual screen is claiming.
  const auto replayed = [&](const gx::UniformReplayLayout& layout) noexcept {
    return (!multiplayer || !layout.nativeEfbEffect) &&
           (layout.perspective || (hudScreen.valid() && !layout.nativeEfbEffect));
  };
  // A 2D rectangle covering the whole frame is one of the game's fades (the black of a respawn, the race
  // start and end wipes, the scene fader, a white flash): on a TV it covers the picture, so in the
  // headset it covers the view. It is not claimed by the virtual screen and keeps its recorded
  // transforms, which the eye replay maps from the frame onto the whole eye (head-locked, like a 2D
  // draw with the virtual screen off). Not in the immersive window, nor split screen.
  // KARTFRAME_FULL_VIEW_FADES=0 leaves them on the screen.
  static const bool fullViewFadesEnabled = [] {
    const char* env = std::getenv("KARTFRAME_FULL_VIEW_FADES");
    return env == nullptr || env[0] != '0';
  }();
  const bool fullViewFades = fullViewFadesEnabled && !stereoFrame.window && !multiplayer && hudScreen.valid() &&
                             displayRegion.width > 0 && displayRegion.height > 0;
  // Only over the scene: a full-frame quad before the pass's first perspective draw is a background (the race
  // draws an opaque one every frame), which stays where it was.
  bool passSawScene = false;
  const auto fullView = [&](const gx::DrawData& draw) noexcept {
    if (!fullViewFades || !passSawScene || draw.uniformReplayLayout.perspective || !draw.screenRect ||
        !draw.fadeCandidate) {
      return false;
    }
    const auto& r = *draw.screenRect;
    const float w = static_cast<float>(displayRegion.width), h = static_cast<float>(displayRegion.height);
    const float x = static_cast<float>(displayRegion.x), y = static_cast<float>(displayRegion.y);
    return r.left <= x + 0.02f * w && r.top <= y + 0.02f * h && r.left + r.width >= x + 0.98f * w &&
           r.top + r.height >= y + 0.98f * h;
  };
  size_t fullViewDrawCount = 0;
  size_t fullViewInvisibleCount = 0;  // of them provably transparent (skipped by the eye replay)
  size_t requiredBytes = 0;
  size_t efbPassCount = 0;
  size_t perspectiveDrawCount = 0;
  size_t replayPerspectiveDrawCount = 0;
  size_t replayHudScreenDrawCount = 0;
  stereo_replay::SubviewRect allocationViewport{float(displayRegion.x), float(displayRegion.y),
                                                float(displayRegion.width), float(displayRegion.height)};
  for (const auto& pass : g_renderPasses) {
    if (pass.efbTarget) {
      ++efbPassCount;
    }
    passSawScene = false;
    for (const auto& command : pass.commands) {
      if (pass.efbTarget && command.type == CommandType::SetViewport) {
        const auto& vp = command.data.setViewport;
        allocationViewport = {vp.left, vp.top, vp.width, vp.height};
      }
      if (command.type != CommandType::Draw || command.data.draw.type != ShaderType::GX ||
          !replayed(command.data.draw.gx.uniformReplayLayout)) {
        continue;
      }
      passSawScene = passSawScene || command.data.draw.gx.uniformReplayLayout.perspective;
      log_full_frame_class(command.data.draw.gx, pass.efbTarget && fullView(command.data.draw.gx), displayRegion);
      if (pass.efbTarget && fullView(command.data.draw.gx)) {
        ++fullViewDrawCount;
        fullViewInvisibleCount += command.data.draw.gx.invisible ? 1 : 0;
        log_replay_quad(command.data.draw.gx, true);
        continue;
      }
      if (pass.efbTarget && !command.data.draw.gx.uniformReplayLayout.perspective) {
        log_replay_quad(command.data.draw.gx, false);
      }
      const auto& draw = command.data.draw.gx;
      const auto& layout = draw.uniformReplayLayout;
      if (layout.perspective) {
        ++perspectiveDrawCount;
      }
      if (!pass.efbTarget) {
        continue;
      }
      if (multiplayer && !stereo_replay::replay_player_one_draw(allocationViewport, playerRegion, layout.perspective,
                                                                layout.nativeEfbEffect)) {
        continue;
      }
      if (layout.perspective) {
        ++replayPerspectiveDrawCount;
      } else {
        ++replayHudScreenDrawCount;
      }
      const size_t projectionEnd = static_cast<size_t>(layout.projectionOffset) + sizeof(Mat4x4<float>);
      const size_t positionEnd = static_cast<size_t>(layout.positionOffset) +
                                 static_cast<size_t>(layout.positionMatrixCount) * sizeof(Mat3x4<float>);
      const size_t normalEnd = static_cast<size_t>(layout.normalOffset) +
                               static_cast<size_t>(layout.normalMatrixCount) * sizeof(Mat3x4<float>);
      if (std::max({projectionEnd, positionEnd, normalEnd}) > draw.uniformRange.size) {
        Log.error(
            "Stereo replay rejected an invalid GX uniform layout (range={}, projection={}, position={}, normal={})",
            draw.uniformRange.size, projectionEnd, positionEnd, normalEnd);
        return false;
      }
      // A 2D screen draw's eye copies carry the clip block (write_screen_clip), padded to the alignment.
      const uint32_t clip = layout.perspective || layout.uniformSize == 0 ? 0u : gx::screen_clip_offset(layout.uniformSize);
      const size_t eyeBytes = clip != 0 ? std::max<size_t>(draw.uniformRange.size, clip + gx::ScreenClipBytes) +
                                              g_cachedLimits.minUniformBufferOffsetAlignment
                                        : draw.uniformRange.size;
      requiredBytes += eyeBytes * AURORA_STEREO_EYE_COUNT;
    }
  }
  // 2D draws on the race's virtual screen, per 10 s (they carry the screen clip).
  static uint64_t hudDrawsWindow = 0, hudFramesWindow = 0, fullViewDrawsWindow = 0, fullViewFramesWindow = 0,
                  fullViewInvisibleWindow = 0;
  fullViewInvisibleWindow += fullViewInvisibleCount;
  static auto hudWindowStart = std::chrono::steady_clock::now();
  hudDrawsWindow += replayHudScreenDrawCount;
  fullViewDrawsWindow += fullViewDrawCount;
  fullViewFramesWindow += fullViewDrawCount != 0 ? 1 : 0;
  ++hudFramesWindow;
  if (const auto now = std::chrono::steady_clock::now(); now - hudWindowStart >= std::chrono::seconds(10)) {
    if (hudScreen.valid()) {
      Log.info("Immersive replay: {:.1f} 2D draws a frame on the virtual screen (10 s); screen clip self-check {:.5f}; "
               "full-view fades: {} draws in {} frames ({} transparent, skipped)",
               static_cast<double>(hudDrawsWindow) / static_cast<double>(std::max<uint64_t>(hudFramesWindow, 1)),
               g_screenClipCheck.load(std::memory_order_relaxed), fullViewDrawsWindow, fullViewFramesWindow,
               fullViewInvisibleWindow);
    }
    hudDrawsWindow = hudFramesWindow = fullViewDrawsWindow = fullViewFramesWindow = fullViewInvisibleWindow = 0;
    hudWindowStart = now;
  }
  static bool replayCoverageLogged = false;
  if (!replayCoverageLogged) {
    replayCoverageLogged = true;
    Log.info(
        "Immersive replay coverage: {} of {} passes target the EFB; {} of {} perspective draws replay; {} draws on the "
        "virtual screen",
        efbPassCount, g_renderPasses.size(), replayPerspectiveDrawCount, perspectiveDrawCount,
        replayHudScreenDrawCount);
  }

  // end_batch_impl appends MaxUniformSize bytes after this for safe dynamic-offset reads.
  if (requiredBytes > UniformBufferSize || g_uniforms.size() > UniformBufferSize - requiredBytes ||
      g_uniforms.size() + requiredBytes > UniformBufferSize - gx::MaxUniformSize) {
    Log.warn("Skipping stereo replay: GX uniform buffer needs {} additional bytes ({} of {} already used)",
             requiredBytes, g_uniforms.size(), UniformBufferSize);
    return false;
  }

  if (history == nullptr) {
    t_eyeScene = {};
  }
  if (history != nullptr) {
    history->uniforms.reserve(replayPerspectiveDrawCount + replayHudScreenDrawCount);
    history->sources.reserve(requiredBytes);
    history->displayRegion = displayRegion;
    history->hudScreen = hudScreen;
    history->uploadOffset =
        static_cast<uint32_t>(AURORA_ALIGN(g_uniforms.size(), g_cachedLimits.minUniformBufferOffsetAlignment));
  }
  Viewport drawViewport{
      .left = static_cast<float>(displayRegion.x),
      .top = static_cast<float>(displayRegion.y),
      .width = static_cast<float>(displayRegion.width),
      .height = static_cast<float>(displayRegion.height),
      .znear = 0.0f,
      .zfar = 1.0f,
  };
  // D3D12 upload heaps can be write-combined. Read each source once, and do
  // all read/modify/write operations in cached CPU memory before uploading.
  std::array<uint8_t, gx::MaxUniformSize> sourceUniform;
  std::array<uint8_t, gx::MaxUniformSize> eyeUniform;
  for (auto& pass : g_renderPasses) {
    pass.cockpitDepth = {};
    if (!pass.efbTarget) {
      continue;
    }
    ClipRect drawScissor{};  // none until the pass sets one
    passSawScene = false;
    for (auto& command : pass.commands) {
      if (command.type == CommandType::SetViewport) {
        drawViewport = command.data.setViewport;
        continue;
      }
      if (command.type == CommandType::SetScissor) {
        const auto& sc = command.data.setScissor;
        drawScissor = {static_cast<int32_t>(sc.x), static_cast<int32_t>(sc.y), static_cast<int32_t>(sc.width),
                       static_cast<int32_t>(sc.height)};
        continue;
      }
      if (command.type != CommandType::Draw || command.data.draw.type != ShaderType::GX ||
          !replayed(command.data.draw.gx.uniformReplayLayout)) {
        continue;
      }
      passSawScene = passSawScene || command.data.draw.gx.uniformReplayLayout.perspective;
      if (fullView(command.data.draw.gx)) {
        continue;
      }
      auto& draw = command.data.draw.gx;
      const auto& layout = draw.uniformReplayLayout;
      const stereo_replay::SubviewRect viewportRect{drawViewport.left, drawViewport.top, drawViewport.width,
                                                    drawViewport.height};
      if (multiplayer && !stereo_replay::replay_player_one_draw(viewportRect, playerRegion, layout.perspective,
                                                                layout.nativeEfbEffect)) {
        continue;
      }
      // Pane-local HUD coordinates expand with P1. Shared race/pause overlays
      // retain their full-screen layout on the virtual screen.
      const bool playerLocal = multiplayer && stereo_replay::subview_contains(playerRegion, viewportRect);
      const ClipRect uniformRegion = playerLocal ? ClipRect{int32_t(playerRegion.left), int32_t(playerRegion.top),
                                                            int32_t(playerRegion.width), int32_t(playerRegion.height)}
                                                 : displayRegion;
      std::memcpy(sourceUniform.data(), g_uniforms.data() + draw.uniformRange.offset, draw.uniformRange.size);
      Mat4x4<float> gameProjection;
      std::memcpy(&gameProjection, sourceUniform.data() + layout.projectionOffset, sizeof(gameProjection));
      // The VR cockpit overlay (hands, synthetic wheel) is drawn in metres and
      // depth-tested against the world, so it needs the world's own depth
      // mapping: the backend depth row of a full-view world draw, with this
      // viewport's depth range folded in because the overlay draws with 0..1.
      // Camera-attached effects share the camera's projection, so any full-view
      // perspective draw describes the same mapping.
      if (layout.perspective && !layout.nativeEfbEffect && gameProjection.m2[3] != 0.0f &&
          drawViewport.width >= displayRegion.width * 0.9f && drawViewport.height >= displayRegion.height * 0.9f) {
        const auto row = stereo_replay::backend_ndc_depth_row(gameProjection);
        const float low = std::clamp(std::min(drawViewport.znear, drawViewport.zfar), 0.f, 1.f);
        const float high = std::clamp(std::max(drawViewport.znear, drawViewport.zfar), 0.f, 1.f);
        pass.cockpitDepth = {row[2] * (high - low) - low, row[3] * (high - low), true};
      }
      // Only a genuinely affine projection carries its NDC position in its clip
      // position, which is what the virtual screen reprojection consumes. GX
      // tracks the projection type separately from the matrix, so a 2D draw
      // whose matrix disagrees keeps its recorded transforms instead of being
      // folded onto the screen from a shape the composition cannot represent.
      if (!layout.perspective && !stereo_replay::is_orthographic_projection(gameProjection)) {
        continue;
      }
      // Producer-side and one-shot, so a mirror-mode race can be confirmed from
      // the log without instrumenting a build.
      static bool mirroredProjectionLogged = false;
      if (layout.perspective && !mirroredProjectionLogged && stereo_replay::projection_mirrors_x(gameProjection)) {
        mirroredProjectionLogged = true;
        Log.info("Immersive replay: perspective draws flip X (mirror mode); mirroring the eye transform to match");
      }
      // A 2D draw on the virtual screen gets the clip block after its own uniform (write_screen_clip).
      const uint32_t clipOffset =
          layout.perspective || layout.uniformSize == 0 ? 0u : gx::screen_clip_offset(layout.uniformSize);
      const uint32_t eyeUniformSize =
          clipOffset != 0 ? std::max(draw.uniformRange.size, clipOffset + gx::ScreenClipBytes) : draw.uniformRange.size;
      LateStereoUniform* saved = nullptr;
      if (history != nullptr) {
        saved = &history->uniforms.emplace_back();
        saved->layout = layout;
        saved->viewport = drawViewport;
        saved->displayRegion = uniformRegion;
        saved->scissor = drawScissor;
        saved->clipOffset = clipOffset;
        const auto save = [&](const uint8_t* source, uint32_t size) -> Range {
          if (size == 0)
            return {};
          const Range copy{static_cast<uint32_t>(history->sources.size()), size};
          history->sources.resize(history->sources.size() + size);
          std::memcpy(history->sources.data() + copy.offset, source, size);
          return copy;
        };
        saved->current = save(sourceUniform.data(), draw.uniformRange.size);
        saved->previous = save(g_uniforms.data() + draw.previousUniformRange.offset, draw.previousUniformRange.size);
      }
      for (uint32_t eyeIndex = 0; eyeIndex < AURORA_STEREO_EYE_COUNT; ++eyeIndex) {
        auto [uniform, range] = map_uniform(eyeUniformSize);
        draw.stereoUniformRanges[eyeIndex] = range;
        if (saved != nullptr) {
          // The late replay uploads these ranges at the actual display time.
          // Preparing another eye pair here would immediately be overwritten.
          saved->eyes[eyeIndex] = range;
          continue;
        }
        const auto& eye = stereoFrame.eyes[eyeIndex];
        if (eyeIndex < 2) {
          auto& scene = t_eyeScene;
          const uint32_t end = range.offset + range.size;
          if (!scene.valid) {
            scene = {.valid = true, .late = false, .weight = 1.f, .fillOffset = range.offset, .fillSize = range.size};
          }
          scene.fillOffset = std::min(scene.fillOffset, range.offset);
          scene.fillSize = std::max(scene.fillOffset + scene.fillSize, end) - scene.fillOffset;
          if (layout.perspective) count_eye_scene(scene.current[eyeIndex]);
        }
        std::memcpy(eyeUniform.data(), sourceUniform.data(), range.size);
        note_eye_pose(eyeIndex, eye, stereoFrame);
        write_stereo_uniform({eyeUniform.data(), range.size}, layout, eye, gameProjection, drawViewport, uniformRegion,
                             hudScreen, false, drawScissor, clipOffset);
        std::memcpy(uniform.data(), eyeUniform.data(), range.size);
        if (layout.perspective) note_eye_probe(eyeIndex, range.offset, {eyeUniform.data(), range.size});
      }
    }
  }
  if (history != nullptr) {
    history->uploadSize = static_cast<uint32_t>(g_uniforms.size()) - history->uploadOffset;
  }
  return true;
}

static bool end_batch_impl(const wgpu::CommandEncoder& cmd, bool advanceFrame,
                           const StereoReplayFrame* stereoFrame = nullptr) {
  ZoneScoped;
  ASSERT(!g_inOffscreen, "end_frame called while offscreen rendering is active");
  if (advanceFrame) {
    gx::finalize_frame_interpolation();
  } else {
    // Mid-frame batch split: the staging buffer is about to be unmapped and rotated, so pending
    // interpolation tasks pointing into it would dangle. Tie the clear to the rotation itself.
    gx::drop_pending_frame_interpolation_uniforms();
  }
  g_pendingLateStereo = {};
  ++g_replayBufferGeneration;
  const bool captureStereo =
      advanceFrame && gx::stereo_frame_interpolation_active() && gx::frame_interpolation_replay_safe();
  const StereoReplayFrame placeholder{};
  const bool stereoPrepared = (stereoFrame == nullptr && !captureStereo) ||
                              prepare_stereo_replay_uniforms(stereoFrame != nullptr ? *stereoFrame : placeholder,
                                                             captureStereo ? &g_pendingLateStereo : nullptr);
  if (captureStereo && stereoPrepared) {
    g_pendingLateStereo.generation = g_replayBufferGeneration.load(std::memory_order_acquire);
  } else {
    g_pendingLateStereo = {};
  }
  g_uniforms.append_zeroes(gx::MaxUniformSize); // Pad the end of the buffer
  uint64_t bufferOffset = 0;
  const auto writeBuffer = [&](ByteBuffer& buf, wgpu::Buffer& out, uint64_t size, std::string_view label) {
    const auto writeSize = buf.size(); // Only need to copy this many bytes
    if (writeSize > 0) {
      cmd.CopyBufferToBuffer(g_stagingBuffers[currentStagingBuffer], bufferOffset, out, 0, AURORA_ALIGN(writeSize, 4));
      buf.release();
    }
    bufferOffset += size;
    return writeSize;
  };
  if (g_uniformUploadDestination != nullptr) {
    // Matching, endpoint capture and all uniform edits are complete. Upload
    // only the used prefix, in one sequential write, before releasing the map.
    std::memcpy(g_uniformUploadDestination, g_uniforms.data(), g_uniforms.size());
    g_uniformUploadDestination = nullptr;
  }
  g_stagingBuffers[currentStagingBuffer].Unmap();
  s_mappingState.store(BufferMapState::Unmapped, std::memory_order_release);
  g_stats.drawCallCount = g_drawCallCount;
  g_stats.mergedDrawCallCount = g_mergedDrawCallCount;
  // The geometry cache's new records ride in the vertex staging (its region starts the staging buffer) and are copied
  // on to the persistent buffer here, before the frame's passes that read them.
  detail::encode_geometry_uploads(cmd, g_stagingBuffers[currentStagingBuffer]);
  gx::geometry_cache::uploads_encoded();
  g_stats.lastVertSize = writeBuffer(g_verts, g_vertexBuffer, VertexBufferSize, "Vertex");
  g_stats.lastUniformSize = writeBuffer(g_uniforms, g_uniformBuffer, UniformBufferSize, "Uniform");
  g_stats.lastIndexSize = writeBuffer(g_indices, g_indexBuffer, IndexBufferSize, "Index");
  // Only this thread raises them; the periodic log line takes them with an exchange.
  if (g_stats.lastVertSize > g_stagingStats.peakVertexBytes.load(std::memory_order_relaxed)) {
    g_stagingStats.peakVertexBytes.store(static_cast<uint32_t>(g_stats.lastVertSize), std::memory_order_relaxed);
  }
  if (g_stats.lastIndexSize > g_stagingStats.peakIndexBytes.load(std::memory_order_relaxed)) {
    g_stagingStats.peakIndexBytes.store(static_cast<uint32_t>(g_stats.lastIndexSize), std::memory_order_relaxed);
  }
  g_stats.lastStorageSize = writeBuffer(g_storage, g_storageBuffer, StorageBufferSize, "Storage");
  if constexpr (UseTextureBuffer) {
    g_stats.lastTextureUploadSize = g_textureUpload.size();
    hitch::g_texUploadBytes.fetch_add(g_textureUpload.size(), std::memory_order_relaxed);
    {
      // Perform texture copies
      for (const auto& item : g_textureUploads) {
        const wgpu::TexelCopyBufferInfo buf{
            .layout =
                wgpu::TexelCopyBufferLayout{
                    .offset = item.layout.offset + bufferOffset,
                    .bytesPerRow = AURORA_ALIGN(item.layout.bytesPerRow, 256),
                    .rowsPerImage = item.layout.rowsPerImage,
                },
            .buffer = g_stagingBuffers[currentStagingBuffer],
        };
        cmd.CopyBufferToTexture(&buf, &item.tex, &item.size);
      }
      g_textureUploads.clear();
      g_textureUpload.release();
    }
  }
  currentStagingBuffer = (currentStagingBuffer + 1) % g_stagingBuffers.size();
  map_staging_buffer();
  g_currentRenderPass = UINT32_MAX;
  for (auto& array : gx::g_gxState.arrays) {
    array.cachedRange = {};
  }
  end_pipeline_frame();
  if (advanceFrame) {
    ++g_frameIndex;
  }
  return stereoPrepared;
}

void end_frame(const wgpu::CommandEncoder& cmd) { (void)end_batch_impl(cmd, true); }

bool end_frame(const wgpu::CommandEncoder& cmd, const StereoReplayFrame& stereoFrame) {
  return end_batch_impl(cmd, true, &stereoFrame);
}

void end_batch(const wgpu::CommandEncoder& cmd) { (void)end_batch_impl(cmd, false); }

uint32_t current_frame() noexcept { return g_frameIndex; }

// The only place that erases from g_cachedBindGroups, whose handles the frame being encoded still
// holds, so it runs in the seal prologue with the renderer mutex held and the producer excluded.
void expire_bind_group_cache() noexcept {
  if (g_cachedBindGroups.empty() || g_frameIndex == UINT32_MAX || g_frameIndex % BindGroupCacheSweepPeriod != 0) {
    return;
  }

  ZoneScoped;
  for (auto it = g_cachedBindGroups.begin(); it != g_cachedBindGroups.end();) {
    if (g_frameIndex - it->second.lastUsedFrame > BindGroupCacheRetainFrames) {
      g_cachedBindGroups.erase(it++);
    } else {
      ++it;
    }
  }
}

// Debug labels for the render passes. Formatting them per pass per slot costs a heap-allocating
// fmt::format for a name nothing reads outside a capture.
static const char* render_pass_label(u32 index) noexcept {
  static constexpr std::array<const char*, 16> kRenderPassLabels{
      "Render pass 0",  "Render pass 1",  "Render pass 2",  "Render pass 3",  "Render pass 4",  "Render pass 5",
      "Render pass 6",  "Render pass 7",  "Render pass 8",  "Render pass 9",  "Render pass 10", "Render pass 11",
      "Render pass 12", "Render pass 13", "Render pass 14", "Render pass 15",
  };
  return index < kRenderPassLabels.size() ? kRenderPassLabels[index] : "Render pass";
}

struct RenderInvocation {
  int32_t interpolatedFrame = -1;
  uint32_t stereoEye = UINT32_MAX;
  const ReplayTarget* target = nullptr;
  ClipRect replaySourceRegion{};
  uint32_t localPlayerCount = 1;
  // Inclusive index of the last pass to replay; -1 replays every pass.
  int32_t replayLastPass = -1;
  // Inclusive index of the last pass that does render work; texture bakes still run for the
  // passes after it. See last_pass_feeding_replay.
  int32_t renderLastPass = INT32_MAX;
  bool finalize = true;
  bool replayOnlyEfb = false;
  bool skipCopyClears = false;
  bool encodeTextureBakes = true;
  bool encodeResolves = true;
  bool captureDepth = true;
  // VR cockpit overlay, drawn inside the scene's pass just before the first
  // virtual-screen draw so the 2D layer's depth cannot hide it (see render_stereo_eye).
  const StereoReplayFrame* cockpitFrame = nullptr;
  wgpu::CommandEncoder* cockpitEncoder = nullptr;
  cockpit::SceneDepth cockpitDepth{};
  bool* cockpitDrawn = nullptr;
  bool* sceneDrawn = nullptr;
  // An eye replay laid out by eye_pass_plan, replacing the one-render-pass-per-recorded-pass loop.
  const eye_pass_plan::Plan* eyePlan = nullptr;
  // Per recorded pass, a scissor every draw of it is clipped to (x, y, width, height; width 0: none).
  const std::vector<std::array<uint32_t, 4>>* passClips = nullptr;
  // The immersive window's mask, drawn by a planned eye at the end of its last render pass
  // (render_stereo_eye draws it otherwise).
  const StereoReplayFrame* windowFrame = nullptr;
  stereo_replay::HudScreen windowScreen{};
  bool* windowDrawn = nullptr;
};

static void render_pass_impl(const wgpu::RenderPassEncoder& pass, const std::vector<RenderPass>& passes, u32 idx,
                             const RenderInvocation& invocation);

// Replays one eye as its plan lays it out. Each step's commands go through render_pass_impl exactly
// as render_impl's loop replays them; only where render passes begin and end differs.
static void render_eye_planned(std::vector<RenderPass>& renderPasses, wgpu::CommandEncoder& cmd,
                               const RenderInvocation& invocation, const eye_pass_plan::Plan& plan) {
  const ReplayTarget& target = *invocation.target;
  const bool stereoStencil = target.depthFormat == wgpu::TextureFormat::Depth24PlusStencil8;
  const GpuTimingCategory timingCategory =
      invocation.stereoEye == 0 ? GpuTimingCategory::EyeLeft : GpuTimingCategory::EyeRight;
  // Foveated only as a single render pass: loading a finished eye back under a density map costs a
  // full tile load per split, which is what made foveation a net loss in DolphinXR.
  const wgpu::TextureView& colorView =
      target.foveatedColorView && plan.renderPasses == 1 ? target.foveatedColorView : target.colorView;
  wgpu::RenderPassEncoder pass;
  for (const auto& step : plan.steps) {
    const auto& passInfo = renderPasses[step.pass];
    if (step.begin) {
      if (pass) {
        pass.End();
      }
      const wgpu::RenderPassColorAttachment colorAttachment{
          .view = colorView,
          .resolveTarget = target.resolveView,
          .loadOp = step.clearColor ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load,
          .storeOp = wgpu::StoreOp::Store,
          .clearValue =
              {
                  .r = passInfo.clearColorValue.x(),
                  .g = passInfo.clearColorValue.y(),
                  .b = passInfo.clearColorValue.z(),
                  .a = passInfo.clearColorValue.w(),
              },
      };
      const wgpu::RenderPassDepthStencilAttachment depthStencilAttachment{
          .view = target.depthView,
          .depthLoadOp = step.clearDepth ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load,
          .depthStoreOp = wgpu::StoreOp::Store,
          .depthClearValue = passInfo.clearDepthValue,
          .stencilLoadOp = stereoStencil ? (step.clearStencil ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load)
                                         : wgpu::LoadOp::Undefined,
          .stencilStoreOp = stereoStencil ? wgpu::StoreOp::Store : wgpu::StoreOp::Undefined,
          .stencilClearValue = 0,
      };
      const wgpu::RenderPassDescriptor renderPassDescriptor{
          .label = render_pass_label(step.pass),
          .colorAttachmentCount = 1,
          .colorAttachments = &colorAttachment,
          .depthStencilAttachment = &depthStencilAttachment,
          .timestampWrites = gpu_timing_pass(timingCategory),
      };
      pass = cmd.BeginRenderPass(&renderPassDescriptor);
    } else {
      // Carry on from the state a fresh render pass starts in: render_pass_impl assumes the whole eye
      // as viewport and scissor, and a clear draw leaves its colour behind as the blend constant.
      const wgpu::Color noBlendConstant{0.0, 0.0, 0.0, 0.0};
      pass.SetViewport(0.0f, 0.0f, static_cast<float>(target.size.width), static_cast<float>(target.size.height),
                       0.0f, 1.0f);
      pass.SetScissorRect(0, 0, target.size.width, target.size.height);
      pass.SetBlendConstant(&noBlendConstant);
      pass.SetStencilReference(0);
    }
    render_pass_impl(pass, renderPasses, step.pass, invocation);
  }
  if (!pass) {
    return;
  }
  // The cockpit that no virtual-screen draw brought in goes over the finished world here rather than
  // in a render pass of its own (render_stereo_eye's fallback), which would load the eye back.
  if (gpu_timing_marks_enabled() && ((invocation.cockpitFrame != nullptr && !*invocation.cockpitDrawn) ||
                                     (invocation.windowFrame != nullptr && !*invocation.windowDrawn))) {
    gpu_timing_mark(pass, GpuTimingCategory::Cockpit);
  }
  if (invocation.cockpitFrame != nullptr && !*invocation.cockpitDrawn) {
    cockpit::render(*invocation.cockpitEncoder, *invocation.cockpitFrame, invocation.stereoEye,
                    invocation.cockpitDepth, &pass);
    *invocation.cockpitDrawn = true;
  }
  // Last of all, over everything the eye draws, in the same render pass.
  if (invocation.windowFrame != nullptr && !*invocation.windowDrawn) {
    window_mask::draw(pass, *invocation.windowFrame, invocation.stereoEye, invocation.windowScreen);
    *invocation.windowDrawn = true;
  }
  pass.End();
}

static void finish_render_impl(std::vector<RenderPass>& renderPasses, const RenderInvocation& invocation) {
  if (invocation.finalize) {
    recycle_render_passes(renderPasses);
  }

#if defined(AURORA_GFX_DEBUG_GROUPS)
  if (invocation.finalize && !g_debugGroupStack.empty()) {
    for (auto& it : std::ranges::reverse_view(g_debugGroupStack)) {
      Log.warn("Debug group was not popped at end of frame: {}", it);
    }
    g_debugGroupStack.clear();
  }

  if (invocation.finalize && g_debugMarkers.size() > 0) {
    g_debugMarkers.clear();
  }
#endif
}

static void render_impl(std::vector<RenderPass>& renderPasses, wgpu::CommandEncoder& cmd,
                        const RenderInvocation& invocation) {
  ZoneScoped;
  if (invocation.eyePlan != nullptr) {
    render_eye_planned(renderPasses, cmd, invocation, *invocation.eyePlan);
    finish_render_impl(renderPasses, invocation);
    return;
  }
  // Palette conversions, MSAA resolves and EFB copies depend on sealed frame state, not on the
  // interpolation weight, so encode them on the native render and let replay slots sample them.
  // Eye textures are reused; discard the previous frame's mask, then retain it
  // across guest passes even if the HUD clears or replaces guest depth.
  bool stencilInitialized = false;
  for (u32 i = 0; i < renderPasses.size(); ++i) {
    const auto& passInfo = renderPasses[i];
    if (invocation.replayLastPass >= 0 && i > static_cast<u32>(invocation.replayLastPass)) {
      // Only immersive replay sets this, and it never encodes bakes or resolves,
      // so nothing later in the list is owed any work.
      break;
    }
    if (invocation.replayOnlyEfb && !passInfo.efbTarget) {
      continue;
    }
    if (invocation.encodeTextureBakes) {
      for (const auto& conv : passInfo.paletteConvs) {
        tex_palette_conv::run(cmd, conv);
      }
    }
    if (static_cast<int32_t>(i) > invocation.renderLastPass) {
      // Nothing after the last replay-feeding resolve is shown or sampled on a headset; the
      // bakes above are all these passes owe the eye replays.
      continue;
    }
    const bool hasRenderWork = passInfo.clearColor || passInfo.clearDepth || !passInfo.commands.empty();
    if (i == renderPasses.size() - 1) {
      ASSERT(!passInfo.resolveTarget, "Final render pass must not have resolve target");
    } else if (!(passInfo.resolveTarget && invocation.encodeResolves) && !hasRenderWork) {
      // Skip only empty intermediate passes: offscreen and scratch passes with resolves still have to
      // run for later samplers, and on a replay slot a resolve-only pass has nothing to encode.
      continue;
    }

    const bool overrideTarget = invocation.target != nullptr && passInfo.efbTarget;
    // A GX copy clear resets the Wii's reused EFB for the next frame. An eye
    // attachment is built fresh per frame and per eye, so reproducing that reset
    // only erases what the replay already drew.
    const bool dropCopyClear = invocation.skipCopyClears && overrideTarget && passInfo.postCopyClear;
    const auto colorView = overrideTarget ? invocation.target->colorView : passInfo.colorView;
    const auto resolveView = overrideTarget ? invocation.target->resolveView : passInfo.resolveView;
    const auto depthView = overrideTarget ? invocation.target->depthView : passInfo.depthView;
    const std::array attachments{
        wgpu::RenderPassColorAttachment{
            .view = colorView,
            .resolveTarget = resolveView,
            .loadOp = passInfo.clearColor && !dropCopyClear ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load,
            .storeOp = wgpu::StoreOp::Store,
            .clearValue =
                {
                    .r = passInfo.clearColorValue.x(),
                    .g = passInfo.clearColorValue.y(),
                    .b = passInfo.clearColorValue.z(),
                    .a = passInfo.clearColorValue.w(),
                },
        },
    };
    const bool stereoStencil = overrideTarget &&
        invocation.target->depthFormat == wgpu::TextureFormat::Depth24PlusStencil8;
    const wgpu::RenderPassDepthStencilAttachment depthStencilAttachment{
        .view = depthView,
        .depthLoadOp = passInfo.clearDepth && !dropCopyClear ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load,
        .depthStoreOp = wgpu::StoreOp::Store,
        .depthClearValue = passInfo.clearDepthValue,
        .stencilLoadOp = stereoStencil ? (stencilInitialized ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear) : wgpu::LoadOp::Undefined,
        .stencilStoreOp = stereoStencil ? wgpu::StoreOp::Store : wgpu::StoreOp::Undefined,
        .stencilClearValue = 0,
    };
    const GpuTimingCategory timingCategory = invocation.stereoEye == 0     ? GpuTimingCategory::EyeLeft
                                             : invocation.stereoEye == 1   ? GpuTimingCategory::EyeRight
                                             : invocation.interpolatedFrame >= 0 ? GpuTimingCategory::Interpolated
                                                                                 : GpuTimingCategory::Mono;
    const wgpu::RenderPassDescriptor renderPassDescriptor{
        .label = render_pass_label(i),
        .colorAttachmentCount = attachments.size(),
        .colorAttachments = attachments.data(),
        .depthStencilAttachment = &depthStencilAttachment,
        .timestampWrites = gpu_timing_pass(timingCategory),
    };

    if (stereoStencil) stencilInitialized = true;
    auto pass = cmd.BeginRenderPass(&renderPassDescriptor);
    render_pass_impl(pass, renderPasses, i, invocation);
    pass.End();

    if (invocation.finalize && invocation.captureDepth && i == renderPasses.size() - 1) {
      depth_peek::encode_frame_snapshot(cmd, passInfo.copySourceDepthView, passInfo.targetSize, passInfo.msaaSamples);
    }

    if (passInfo.resolveTarget && invocation.encodeResolves) {
      const bool isDepth = gx::is_depth_format(passInfo.resolveFormat);
      wgpu::Texture resolveSourceTexture = passInfo.copySourceTexture;
      wgpu::TextureView resolveSourceView = isDepth ? passInfo.copySourceDepthView : passInfo.copySourceView;
      if (passInfo.snapshotColorResolveSource && passInfo.resolveSourceSnapshot) {
        const wgpu::TexelCopyTextureInfo src{
            .texture = passInfo.copySourceTexture,
            .origin =
                wgpu::Origin3D{
                    .x = static_cast<uint32_t>(passInfo.resolveSnapshotRect.x),
                    .y = static_cast<uint32_t>(passInfo.resolveSnapshotRect.y),
                },
        };
        const wgpu::TexelCopyTextureInfo dst{
            .texture = passInfo.resolveSourceSnapshot->texture,
        };
        const wgpu::Extent3D size{
            .width = static_cast<uint32_t>(passInfo.resolveSnapshotRect.width),
            .height = static_cast<uint32_t>(passInfo.resolveSnapshotRect.height),
            .depthOrArrayLayers = 1,
        };
        cmd.CopyTextureToTexture(&src, &dst, &size);
        resolveSourceTexture = passInfo.resolveSourceSnapshot->texture;
        resolveSourceView = passInfo.resolveSourceSnapshot->sampleTextureView;
      }
      if (isDepth && passInfo.msaaSamples > 1) {
        Log.fatal("Depth tex copies from multisampled EFB targets are not supported");
      }
      const tex_copy_conv::ConvRequest convReq{
          .fmt = passInfo.resolveFormat,
          .srcView = resolveSourceView,
          .uniformRange = passInfo.resolveUniformRange,
          .dst = passInfo.resolveTarget,
          .sampleFilter = passInfo.resolveLinearSampling ? tex_copy_conv::SampleFilter::Linear
                                                         : tex_copy_conv::SampleFilter::Nearest,
          .forceOpaqueAlpha = passInfo.resolveForceOpaqueAlpha,
      };
      if (passInfo.resolveNeedsConversion) {
        tex_copy_conv::run(cmd, convReq);
      } else if (passInfo.resolveNeedsShaderSampling) {
        tex_copy_conv::blit(cmd, convReq);
      } else {
        const wgpu::TexelCopyTextureInfo src{
            .texture = resolveSourceTexture,
            .origin =
                wgpu::Origin3D{
                    .x = static_cast<uint32_t>(passInfo.resolveRect.x - (passInfo.snapshotColorResolveSource
                                                                             ? passInfo.resolveSnapshotRect.x
                                                                             : 0)),
                    .y = static_cast<uint32_t>(passInfo.resolveRect.y - (passInfo.snapshotColorResolveSource
                                                                             ? passInfo.resolveSnapshotRect.y
                                                                             : 0)),
                },
        };
        const wgpu::TexelCopyTextureInfo dst{
            .texture = passInfo.resolveTarget->texture,
        };
        const wgpu::Extent3D size{
            .width = static_cast<uint32_t>(passInfo.resolveRect.width),
            .height = static_cast<uint32_t>(passInfo.resolveRect.height),
            .depthOrArrayLayers = 1,
        };
        cmd.CopyTextureToTexture(&src, &dst, &size);
      }
    }
  }
  finish_render_impl(renderPasses, invocation);
}

void seal_frame(SealedFrame& out) noexcept {
  ZoneScoped;
  // The encode that could still have been holding these has completed: the
  // producer joins the worker's DONE phase before it seals another frame.
  g_retiredBindGroups.clear();
  out.data().stereo = std::move(g_pendingLateStereo);
  out.data().localPlayerCount = g_stereoLocalPlayerCount;
  g_stereoLocalPlayerCount = 1;
  auto& passes = out.data().passes;
  // The previous cycle already recycled these, so this normally just hands the empty vector, its
  // capacity included, back to the producer.
  recycle_render_passes(passes);
  passes.swap(g_renderPasses);
  g_currentRenderPass = UINT32_MAX;
  out.data().windowScreen = stereo_window_screen();
}

// A native render cut after `last` (an immersive frame: only the copies the eyes sample matter) needs
// its pixels only where those copies read. In a Mario Kart race that is a 6x4 depth copy at the sun
// for the lens flare, after the whole scene: without the clip the native render drew the full 3D
// scene at 939x528 for it, about 1 ms of GPU a game frame (lane I). Each pass gets the bounds of the
// copies it and the passes after it make (its pixels stay in the EFB for them), plus a margin for the
// copy filter; the vertex work stays. KARTFRAME_NATIVE_CLIP=0 draws the whole passes again.
static const std::vector<std::array<uint32_t, 4>>* native_cut_clips(const std::vector<RenderPass>& passes,
                                                                    int32_t last) {
  static const bool enabled = [] {
    const char* env = std::getenv("KARTFRAME_NATIVE_CLIP");
    return env == nullptr || env[0] != '0';
  }();
  if (!enabled || last < 0 || static_cast<size_t>(last) >= passes.size()) {
    return nullptr;
  }
  thread_local std::vector<std::array<uint32_t, 4>> clips;
  clips.assign(passes.size(), {0, 0, 0, 0});
  constexpr int32_t kMargin = 2;
  int32_t left = INT32_MAX, top = INT32_MAX, right = INT32_MIN, bottom = INT32_MIN;
  for (int32_t i = last; i >= 0; --i) {
    const auto& pass = passes[static_cast<size_t>(i)];
    if (pass.resolveTarget && !pass.displayCopyResolve) {
      const auto& r = pass.snapshotColorResolveSource ? pass.resolveSnapshotRect : pass.resolveRect;
      const auto& q = pass.resolveRect;
      left = std::min({left, r.x, q.x});
      top = std::min({top, r.y, q.y});
      right = std::max({right, r.x + r.width, q.x + q.width});
      bottom = std::max({bottom, r.y + r.height, q.y + q.height});
    }
    if (right <= left || bottom <= top || !pass.efbTarget) {
      continue;  // nothing read yet; an offscreen pass draws whole (it is a texture of its own)
    }
    const int32_t width = static_cast<int32_t>(pass.targetSize.width);
    const int32_t height = static_cast<int32_t>(pass.targetSize.height);
    const int32_t x0 = std::clamp(left - kMargin, 0, width), y0 = std::clamp(top - kMargin, 0, height);
    const int32_t x1 = std::clamp(right + kMargin, x0, width), y1 = std::clamp(bottom + kMargin, y0, height);
    if (x1 > x0 && y1 > y0) {
      clips[static_cast<size_t>(i)] = {static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
                                       static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)};
    }
  }
  return &clips;
}

void render(SealedFrame& frame, wgpu::CommandEncoder& cmd, int32_t interpolatedFrame, bool finalize,
            int32_t nativeRenderLastPass) {
  const bool cut = interpolatedFrame < 0 && nativeRenderLastPass != INT32_MAX;
  render_impl(frame.data().passes, cmd,
              RenderInvocation{
                  .interpolatedFrame = interpolatedFrame,
                  .renderLastPass = nativeRenderLastPass,
                  .finalize = finalize,
                  .encodeTextureBakes = interpolatedFrame < 0,
                  .passClips = cut ? native_cut_clips(frame.data().passes, nativeRenderLastPass) : nullptr,
              });
}

int32_t last_pass_feeding_replay(const SealedFrame& frame) noexcept {
  const auto& passes = frame.data().passes;
  int32_t last = -1;
  for (size_t i = 0; i < passes.size(); ++i) {
    if (passes[i].resolveTarget && !passes[i].displayCopyResolve) {
      last = static_cast<int32_t>(i);
    }
  }
  // Each distinct pass layout once: what the native render of an immersive frame still has to draw.
  static std::mutex loggedMutex;
  static std::vector<std::string> logged;
  std::string shape;
  for (size_t i = 0; i < passes.size(); ++i) {
    const auto& pass = passes[i];
    size_t draws = 0, perspective = 0;
    for (const auto& command : pass.commands) {
      if (command.type == CommandType::Draw && command.data.draw.type == ShaderType::GX) {
        ++draws;
        perspective += command.data.draw.gx.uniformReplayLayout.perspective ? 1 : 0;
      }
    }
    shape += fmt::format(" [{}{} {}x{} draws {} (3D {})", i, pass.efbTarget ? " efb" : "", pass.targetSize.width,
                         pass.targetSize.height, draws, perspective);
    if (pass.resolveTarget) {
      shape += fmt::format(" -> copy fmt {} {}x{}{}{}", static_cast<int>(pass.resolveFormat), pass.resolveRect.width,
                           pass.resolveRect.height, pass.resolveHalfScale ? " half" : "",
                           pass.displayCopyResolve ? " display" : "");
    }
    shape += "]";
  }
  std::lock_guard lock(loggedMutex);
  std::string key;
  for (char c : shape) key += std::isdigit(static_cast<unsigned char>(c)) ? '#' : c;
  if (logged.size() < 24 && std::ranges::find(logged, key) == logged.end()) {
    logged.push_back(key);
    Log.info("Native render of an immersive frame stops after pass {} of {}:{}", last, passes.size(), shape);
  }
  return last;
}

bool has_late_stereo_replay(const SealedFrame& frame) noexcept {
  const auto& data = frame.data().stereo;
  return data.generation != 0 && data.generation == g_replayBufferGeneration.load(std::memory_order_acquire) &&
         !data.uniforms.empty();
}

bool prepare_late_stereo_replay(SealedFrame& frame, wgpu::CommandEncoder& cmd, const StereoReplayFrame& stereoFrame,
                                float weight) {
  if (!has_late_stereo_replay(frame))
    return false;
  auto& data = frame.data().stereo;
  if (static auto lastCheck = std::chrono::steady_clock::time_point{};
      data.hudScreen.valid() && std::chrono::steady_clock::now() - lastCheck >= std::chrono::seconds(5)) {
    lastCheck = std::chrono::steady_clock::now();
    g_screenClipCheck.store(screen_clip_self_check(stereoFrame.eyes[0], data.hudScreen), std::memory_order_relaxed);
  }
  data.uploadBytes.resize(data.uploadSize);
  auto* bytes = data.uploadBytes.data();
  weight = std::clamp(weight, 0.0f, 1.0f);
  auto& scene = t_eyeScene;
  scene = {.valid = true, .late = true, .anchored = stereoFrame.interpolateInAnchor, .weight = weight,
           .fillOffset = data.uploadOffset, .fillSize = data.uploadSize};
  for (const auto& saved : data.uniforms) {
    // Interpolate once; both eyes share exactly the same scene sample.
    std::span<uint8_t> uniform{bytes + saved.eyes[0].offset - data.uploadOffset, saved.current.size};
    std::memcpy(uniform.data(), data.sources.data() + saved.current.offset, uniform.size());
    const auto& layout = saved.layout;
    const bool hasPrevious = saved.previous.size == saved.current.size;
    if (layout.perspective && ((hasPrevious && weight < 1.0f) || stereoFrame.interpolateInAnchor)) {
      const auto* previous = data.sources.data() + (hasPrevious ? saved.previous.offset : saved.current.offset);
      const auto interpolateMatrices = [&](uint32_t offset, uint32_t count, uint32_t mask, bool normal) {
        for (uint32_t matrix = 0; matrix < count; ++matrix) {
          if ((mask & (1u << matrix)) == 0)
            continue;
          const size_t at = offset + matrix * sizeof(Mat3x4<float>);
          Mat3x4<float> before, current, result;
          std::memcpy(&before, previous + at, sizeof(before));
          std::memcpy(&current, uniform.data() + at, sizeof(current));
          const auto interpolate = [&](const auto& a, const auto& b, float t, auto& out) {
            return layout.indexedMatrices ? gx::interpolate_indexed_transform(a, b, t, out)
                                          : gx::interpolate_transform(a, b, t, out);
          };
          bool valid = true;
          if (stereoFrame.interpolateInAnchor) {
            result = stereo_replay::interpolate_anchored_transform(before, current,
                stereoFrame.previousAnchor, stereoFrame.currentAnchor,
                hasPrevious ? weight : 1.f, normal, interpolate);
          } else {
            valid = interpolate(before, current, weight, result);
          }
          if (valid)
            std::memcpy(uniform.data() + at, &result, sizeof(result));
        }
      };
      interpolateMatrices(layout.positionOffset, layout.positionMatrixCount, layout.positionMatrixMask, false);
      interpolateMatrices(layout.normalOffset, layout.normalMatrixCount, layout.positionMatrixMask, true);
      // Interpolate the game depth mapping before applying the HMD frustum.
      for (size_t component = 0; component < 16; ++component) {
        const size_t at = layout.projectionOffset + component * sizeof(float);
        float before, current;
        std::memcpy(&before, previous + at, sizeof(float));
        std::memcpy(&current, uniform.data() + at, sizeof(float));
        const float value = before + (current - before) * (hasPrevious ? weight : 1.f);
        std::memcpy(uniform.data() + at, &value, sizeof(float));
      }
    }
    Mat4x4<float> projection;
    std::memcpy(&projection, uniform.data() + layout.projectionOffset, sizeof(projection));
    for (uint32_t eye = 1; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
      std::memcpy(bytes + saved.eyes[eye].offset - data.uploadOffset, uniform.data(), uniform.size());
    }
    const bool interpolated = layout.perspective && hasPrevious && weight < 1.0f;
    for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
      note_eye_pose(eye, stereoFrame.eyes[eye], stereoFrame);
      write_stereo_uniform({bytes + saved.eyes[eye].offset - data.uploadOffset, saved.eyes[eye].size}, layout,
                           stereoFrame.eyes[eye], projection, saved.viewport, saved.displayRegion, data.hudScreen,
                           stereoFrame.interpolateInAnchor, saved.scissor, saved.clipOffset);
      if (layout.perspective)
        note_eye_probe(eye, saved.eyes[eye].offset, {bytes + saved.eyes[eye].offset - data.uploadOffset, saved.eyes[eye].size});
      if (layout.perspective && eye < 2) count_eye_scene(interpolated ? scene.interpolated[eye] : scene.current[eye]);
    }
  }
  // Never interpolate in mapped upload memory: write-combined pages make CPU
  // reads expensive even for values that were just written there.
  const wgpu::BufferDescriptor descriptor{
      .label = "Headset interpolation uniforms",
      .usage = wgpu::BufferUsage::CopySrc,
      .size = data.uploadSize,
      .mappedAtCreation = true,
  };
  auto upload = g_device.CreateBuffer(&descriptor);
  std::memcpy(upload.GetMappedRange(), bytes, data.uploadSize);
  upload.Unmap();
  // Command-buffer ordering keeps these writes after the preceding eye pair,
  // without mutating any producer staging memory or desktop uniforms.
  gpu_timing_mark(cmd, GpuTimingCategory::Upload);
  cmd.CopyBufferToBuffer(upload, 0, g_uniformBuffer, data.uploadOffset, data.uploadSize);
  gpu_timing_mark(cmd, std::nullopt);
  return true;
}

// This frame's eye plan, built in scratch storage that is reused from frame to frame. Each new plan
// shape is logged once, so a headset log shows the pass structure of every scene it went through.
static const eye_pass_plan::Plan& plan_eye_passes(const std::vector<RenderPass>& passes, int32_t lastPass,
                                                  bool skipCopyClears) {
  thread_local std::vector<eye_pass_plan::PassSummary> summaries;
  thread_local eye_pass_plan::Plan plan;
  summaries.resize(passes.size());
  for (size_t i = 0; i < passes.size(); ++i) {
    const auto& pass = passes[i];
    summaries[i] = eye_pass_plan::PassSummary{
        .efbTarget = pass.efbTarget,
        .clearColor = pass.clearColor,
        .clearDepth = pass.clearDepth,
        .postCopyClear = pass.postCopyClear,
        .hasCommands = !pass.commands.empty(),
    };
  }
  eye_pass_plan::build(summaries.data(), summaries.size(), lastPass, skipCopyClears, plan);

  static std::mutex loggedShapesMutex;
  static std::vector<std::array<uint32_t, 5>> loggedShapes;
  const std::array<uint32_t, 5> shape{plan.efbPasses, plan.renderPasses, plan.dead, plan.empty, plan.splits};
  std::lock_guard lock{loggedShapesMutex};
  if (loggedShapes.size() < 32 && std::ranges::find(loggedShapes, shape) == loggedShapes.end()) {
    loggedShapes.push_back(shape);
    Log.info("Eye replay plan: {} EFB passes -> {} render pass{} per eye ({} erased by a later clear, {} empty, "
             "{} split by a partial clear)",
             plan.efbPasses, plan.renderPasses, plan.renderPasses == 1 ? "" : "es", plan.dead, plan.empty,
             plan.splits);
  }
  return plan;
}

void render_stereo_eye(SealedFrame& frame, wgpu::CommandEncoder& cmd, const StereoReplayFrame& stereoFrame,
                       uint32_t eye, bool finalize) {
  CHECK(eye < AURORA_STEREO_EYE_COUNT, "invalid stereo eye {}", eye);
  const auto displaySource = stereo_display_source(frame.data().passes);
  // GXCopyDisp publishes the frame and then clears the EFB for the next one.
  // The eye is a fresh per-frame attachment, not the reused EFB, so replaying
  // past that copy blanks the very image the game presented.
  const int32_t lastPass = get_stereo_stop_at_display_copy() ? displaySource.lastDisplayCopyPass : -1;
  cockpit::SceneDepth cockpitDepth{};
  for (size_t i = 0; i < frame.data().passes.size(); ++i) {
    if (lastPass >= 0 && i > static_cast<size_t>(lastPass)) {
      break;
    }
    if (frame.data().passes[i].cockpitDepth.valid) {
      cockpitDepth = frame.data().passes[i].cockpitDepth;
    }
  }
  bool cockpitDrawn = false;
  bool sceneDrawn = false;
  bool windowDrawn = false;
  const bool cockpitActive = stereoFrame.cockpit.active && cockpitDepth.valid;
  const bool skipCopyClears = get_stereo_skip_copy_clears();
  const eye_pass_plan::Plan* plan =
      get_stereo_single_pass_eyes() ? &plan_eye_passes(frame.data().passes, lastPass, skipCopyClears) : nullptr;
  render_impl(frame.data().passes, cmd,
              RenderInvocation{
                  .stereoEye = eye,
                  .target = &stereoFrame.eyes[eye].target,
                  .replaySourceRegion = displaySource.region,
                  .localPlayerCount = frame.data().localPlayerCount,
                  .replayLastPass = lastPass,
                  .finalize = finalize,
                  .replayOnlyEfb = true,
                  .skipCopyClears = skipCopyClears,
                  .encodeTextureBakes = false,
                  .encodeResolves = false,
                  .captureDepth = false,
                  .cockpitFrame = cockpitActive ? &stereoFrame : nullptr,
                  .cockpitEncoder = &cmd,
                  .cockpitDepth = cockpitDepth,
                  .cockpitDrawn = &cockpitDrawn,
                  .sceneDrawn = &sceneDrawn,
                  .eyePlan = plan,
                  .windowFrame = stereoFrame.window ? &stereoFrame : nullptr,
                  .windowScreen = frame.data().windowScreen,
                  .windowDrawn = &windowDrawn,
              });
  // A frame without a virtual-screen draw after its world still gets the
  // overlay, in a pass of its own over the finished eye. A planned eye draws it
  // in its last render pass instead, unless it had nothing to render at all.
  if (cockpitActive && !cockpitDrawn) {
    cockpit::render(cmd, stereoFrame, eye, cockpitDepth);
  }
  // Likewise the window's mask, which has to come after the cockpit.
  if (stereoFrame.window && !windowDrawn) {
    window_mask::render(cmd, stereoFrame, eye, frame.data().windowScreen);
  }
}

void mask_stereo_eye_output(const SealedFrame& frame, wgpu::CommandEncoder& cmd, const StereoReplayFrame& stereoFrame,
                            uint32_t eye, const wgpu::TextureView& output, wgpu::Extent3D size) {
  CHECK(eye < AURORA_STEREO_EYE_COUNT, "invalid stereo eye {}", eye);
  window_mask::render_output(cmd, stereoFrame, eye, output, size, frame.data().windowScreen);
}

void render(wgpu::CommandEncoder& cmd, int32_t interpolatedFrame, bool finalize) {
  render_impl(g_renderPasses, cmd,
              RenderInvocation{
                  .interpolatedFrame = interpolatedFrame,
                  .finalize = finalize,
                  .encodeTextureBakes = interpolatedFrame < 0,
              });
  if (finalize) {
    g_currentRenderPass = UINT32_MAX;
    expire_bind_group_cache();
  }
}

// --- Per-pass GPU timing (see common.hpp) -------------------------------------------------------
namespace {
constexpr uint32_t kGpuTimingSlots = 6;
constexpr uint32_t kGpuTimingQueries = 512;
constexpr uint32_t kGpuTimingPasses = 128;
constexpr uint8_t kGpuTimingEnd = 0xff;
constexpr size_t kGpuTimingCategories = static_cast<size_t>(GpuTimingCategory::Count);
constexpr size_t kGpuTimingKinds = 2;

struct GpuTimingEvent {
  uint32_t query;
  uint8_t category;  // the segment this timestamp begins; kGpuTimingEnd ends one
};

struct GpuTimingSlot {
  wgpu::QuerySet querySet;
  wgpu::Buffer resolve;
  wgpu::Buffer readback;
  std::array<wgpu::PassTimestampWrites, kGpuTimingPasses> writes{};
  std::vector<GpuTimingEvent> events;
  uint32_t passes = 0;
  uint32_t queries = 0;
  uint8_t kind = 0;
  bool open = false;    // between the frame's begin and end
  bool reading = false; // readback in flight or mapped
  bool mapped = false;  // the callback ran; the encoding thread unmaps on reuse
};

struct GpuTimingTotals {
  std::array<uint64_t, kGpuTimingCategories> ns{};
  uint64_t betweenNs = 0;
  std::vector<float> spanMs;
  uint32_t frames = 0;
};

std::atomic<bool> g_gpuTimingEnabled{false};
std::array<GpuTimingSlot, kGpuTimingSlots> g_gpuTimingSlots;
uint32_t g_gpuTimingNextSlot = 0;
int32_t g_gpuTimingCurrent = -1;
bool g_gpuTimingReady = false;
// Guards the totals below and every slot's reading/mapped flags: the map callback may run on
// whichever thread processes Dawn's events.
std::mutex g_gpuTimingMutex;
std::array<GpuTimingTotals, kGpuTimingKinds> g_gpuTimingTotals;
uint32_t g_gpuTimingSkipped = 0;

bool gpu_timing_create_slots() {
  if (g_gpuTimingReady) {
    return true;
  }
  if (!webgpu::g_timestampQueriesSupported || !webgpu::g_device) {
    return false;
  }
  for (auto& slot : g_gpuTimingSlots) {
    const wgpu::QuerySetDescriptor querySetDescriptor{
        .label = "GPU timing queries",
        .type = wgpu::QueryType::Timestamp,
        .count = kGpuTimingQueries,
    };
    slot.querySet = webgpu::g_device.CreateQuerySet(&querySetDescriptor);
    const wgpu::BufferDescriptor resolveDescriptor{
        .label = "GPU timing resolve",
        .usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc,
        .size = kGpuTimingQueries * sizeof(uint64_t),
    };
    slot.resolve = webgpu::g_device.CreateBuffer(&resolveDescriptor);
    const wgpu::BufferDescriptor readbackDescriptor{
        .label = "GPU timing readback",
        .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
        .size = kGpuTimingQueries * sizeof(uint64_t),
    };
    slot.readback = webgpu::g_device.CreateBuffer(&readbackDescriptor);
    slot.events.reserve(kGpuTimingQueries);
  }
  g_gpuTimingReady = true;
  return true;
}

GpuTimingSlot* gpu_timing_open_slot() noexcept {
  if (g_gpuTimingCurrent < 0) {
    return nullptr;
  }
  auto& slot = g_gpuTimingSlots[static_cast<size_t>(g_gpuTimingCurrent)];
  return slot.open ? &slot : nullptr;
}
} // namespace

void gpu_timing_set_enabled(bool enabled) noexcept { g_gpuTimingEnabled.store(enabled, std::memory_order_relaxed); }
bool gpu_timing_enabled() noexcept { return g_gpuTimingEnabled.load(std::memory_order_relaxed); }

void gpu_timing_begin_frame(uint8_t kind) noexcept {
  g_gpuTimingCurrent = -1;
  if (!gpu_timing_enabled() || !gpu_timing_create_slots()) {
    return;
  }
  const uint32_t index = g_gpuTimingNextSlot;
  g_gpuTimingNextSlot = (g_gpuTimingNextSlot + 1) % kGpuTimingSlots;
  auto& slot = g_gpuTimingSlots[index];
  {
    std::lock_guard lock(g_gpuTimingMutex);
    if (slot.reading && !slot.mapped) {
      ++g_gpuTimingSkipped; // the GPU is more than a ring behind; leave this frame untimed
      return;
    }
    if (slot.mapped) {
      slot.readback.Unmap();
      slot.mapped = false;
    }
    slot.reading = false;
  }
  slot.passes = 0;
  slot.queries = 0;
  slot.events.clear();
  slot.kind = static_cast<uint8_t>(std::min<size_t>(kind, kGpuTimingKinds - 1));
  slot.open = true;
  g_gpuTimingCurrent = static_cast<int32_t>(index);
}

const wgpu::PassTimestampWrites* gpu_timing_pass(GpuTimingCategory category) noexcept {
  auto* slot = gpu_timing_open_slot();
  if (slot == nullptr || slot->passes >= kGpuTimingPasses || slot->queries + 2 > kGpuTimingQueries) {
    return nullptr;
  }
  const uint32_t q = slot->queries;
  slot->queries += 2;
  auto& writes = slot->writes[slot->passes++];
  writes = wgpu::PassTimestampWrites{
      .querySet = slot->querySet,
      .beginningOfPassWriteIndex = q,
      .endOfPassWriteIndex = q + 1,
  };
  slot->events.push_back({q, static_cast<uint8_t>(category)});
  slot->events.push_back({q + 1, kGpuTimingEnd});
  return &writes;
}

bool gpu_timing_marks_enabled() noexcept {
  return webgpu::g_timestampInsidePassesSupported && gpu_timing_open_slot() != nullptr;
}

void gpu_timing_mark(const wgpu::RenderPassEncoder& pass, GpuTimingCategory category) noexcept {
  auto* slot = gpu_timing_open_slot();
  if (slot == nullptr || !webgpu::g_timestampInsidePassesSupported || slot->queries + 1 > kGpuTimingQueries) {
    return;
  }
  const uint32_t q = slot->queries++;
  pass.WriteTimestamp(slot->querySet, q);
  slot->events.push_back({q, static_cast<uint8_t>(category)});
}

void gpu_timing_mark(const wgpu::CommandEncoder& encoder, std::optional<GpuTimingCategory> category) noexcept {
  auto* slot = gpu_timing_open_slot();
  if (slot == nullptr || !webgpu::g_timestampInsidePassesSupported || slot->queries + 1 > kGpuTimingQueries) {
    return;
  }
  const uint32_t q = slot->queries++;
  encoder.WriteTimestamp(slot->querySet, q);
  slot->events.push_back({q, category ? static_cast<uint8_t>(*category) : kGpuTimingEnd});
}

void gpu_timing_end_frame(wgpu::CommandEncoder& encoder) noexcept {
  auto* slot = gpu_timing_open_slot();
  if (slot == nullptr) {
    g_gpuTimingCurrent = -1;
    return;
  }
  slot->open = false;
  if (slot->queries == 0) {
    g_gpuTimingCurrent = -1;
    return;
  }
  encoder.ResolveQuerySet(slot->querySet, 0, slot->queries, slot->resolve, 0);
  encoder.CopyBufferToBuffer(slot->resolve, 0, slot->readback, 0, slot->queries * sizeof(uint64_t));
}

void gpu_timing_after_submit() noexcept {
  if (g_gpuTimingCurrent < 0) {
    return;
  }
  const uint32_t index = static_cast<uint32_t>(g_gpuTimingCurrent);
  g_gpuTimingCurrent = -1;
  auto& slot = g_gpuTimingSlots[index];
  const uint32_t queries = slot.queries;
  {
    std::lock_guard lock(g_gpuTimingMutex);
    slot.reading = true;
    slot.mapped = false;
  }
  slot.readback.MapAsync(
      wgpu::MapMode::Read, 0, queries * sizeof(uint64_t), wgpu::CallbackMode::AllowSpontaneous,
      [index, queries](wgpu::MapAsyncStatus status, wgpu::StringView) {
        auto& slot = g_gpuTimingSlots[index];
        std::lock_guard lock(g_gpuTimingMutex);
        if (status != wgpu::MapAsyncStatus::Success) {
          slot.reading = false;
          return;
        }
        const auto* stamps = static_cast<const uint64_t*>(slot.readback.GetConstMappedRange(0, queries * sizeof(uint64_t)));
        if (stamps != nullptr) {
          // Timestamps on one queue follow execution order, so sorting by value recovers the order of
          // pass begins, marks and pass ends however their query indices were handed out. A query
          // nothing wrote (a pass that was never encoded) resolves to 0 and is left out.
          thread_local std::vector<std::pair<uint64_t, uint8_t>> ordered;
          ordered.clear();
          for (const auto& event : slot.events) {
            if (event.query < queries && stamps[event.query] != 0) {
              ordered.emplace_back(stamps[event.query], event.category);
            }
          }
          std::stable_sort(ordered.begin(), ordered.end(),
                           [](const auto& a, const auto& b) { return a.first < b.first; });
          auto& totals = g_gpuTimingTotals[slot.kind];
          for (size_t i = 0; i + 1 < ordered.size(); ++i) {
            const uint64_t span = ordered[i + 1].first - ordered[i].first;
            if (ordered[i].second == kGpuTimingEnd) {
              totals.betweenNs += span;
            } else if (ordered[i].second < kGpuTimingCategories) {
              totals.ns[ordered[i].second] += span;
            }
          }
          if (ordered.size() >= 2) {
            totals.spanMs.push_back(static_cast<float>(ordered.back().first - ordered.front().first) / 1e6f);
          }
          ++totals.frames;
        }
        slot.mapped = true;
      });
}

std::string gpu_timing_report() {
  std::lock_guard lock(g_gpuTimingMutex);
  static constexpr std::array<const char*, kGpuTimingCategories> kNames{
      "mono",  "eyeL-load", "eyeR-load", "interp",  "screen",   "panel",  "efbcopy", "palette",   "peek",
      "snapshot", "present", "scene",  "hud2d",  "fade",  "efbfx",  "clear",  "other2d", "cockpit", "upload",
      "sink", "hudlayer", "menuenv"};
  static constexpr std::array<const char*, kGpuTimingKinds> kKinds{"game-frame sets", "refresh sets"};
  std::string text;
  for (size_t kind = 0; kind < kGpuTimingKinds; ++kind) {
    auto& totals = g_gpuTimingTotals[kind];
    if (totals.frames == 0) {
      continue;
    }
    const double frames = totals.frames;
    auto& spans = totals.spanMs;
    std::sort(spans.begin(), spans.end());
    const auto pct = [&](double p) {
      return spans.empty() ? 0.0 : static_cast<double>(spans[std::min(spans.size() - 1, size_t(p * double(spans.size() - 1) + 0.5))]);
    };
    text += fmt::format("{}GPU ms per {} ({}): span p50/p99 {:.2f}/{:.2f}", text.empty() ? "" : "; ", kKinds[kind],
                        totals.frames, pct(0.5), pct(0.99));
    for (size_t i = 0; i < kNames.size(); ++i) {
      if (totals.ns[i] != 0) {
        text += fmt::format(" {}={:.3f}", kNames[i], static_cast<double>(totals.ns[i]) / 1e6 / frames);
      }
    }
    text += fmt::format(" between={:.3f}", static_cast<double>(totals.betweenNs) / 1e6 / frames);
    totals = {};
  }
  if (g_gpuTimingSkipped != 0) {
    text += fmt::format(" (untimed frames: {})", g_gpuTimingSkipped);
  }
  g_gpuTimingSkipped = 0;
  return text;
}

void after_submit() noexcept {
  depth_peek::after_submit();
  efb_ram::after_submit();
  // Retire this frame's completed GPU work. Dawn only reclaims destroyed resources inside a device
  // tick, and a frame that never ticks keeps every released image and its memory for the run.
  if (g_instance) {
    g_instance.ProcessEvents();
  }
}

static void render_pass_impl(const wgpu::RenderPassEncoder& pass, const std::vector<RenderPass>& renderPasses, u32 idx,
                             const RenderInvocation& invocation) {
  // Per-invocation, not per-process: two encoders can be recording at once.
  gx::DrawEncodeState encodeState{};
  encodeState.boundTextureBindGroup = gx::g_emptyTextureBindGroup.Get();
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> lastDebugGroupStack;
#endif

  // Bind static bind group for the whole pass
  pass.SetBindGroup(0, g_staticBindGroup);
  pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);

  const auto& sourceSize = renderPasses[idx].targetSize;
  const bool overrideTarget = invocation.target != nullptr && renderPasses[idx].efbTarget;
  const auto targetSize = overrideTarget ? invocation.target->size : sourceSize;
  const bool multiplayer = overrideTarget && invocation.localPlayerCount > 1;
  const auto& display = invocation.replaySourceRegion;
  const auto playerRegion = stereo_replay::player_one_region(
      {float(display.x), float(display.y), float(display.width), float(display.height)}, invocation.localPlayerCount);
  stereo_replay::SubviewRect sourceViewport{0.f, 0.f, float(sourceSize.width), float(sourceSize.height)};
  auto sourceScissor = sourceViewport;
  const int32_t sourceWidth = static_cast<int32_t>(sourceSize.width);
  const int32_t sourceHeight = static_cast<int32_t>(sourceSize.height);
  int32_t sourceRegionLeft = 0;
  int32_t sourceRegionTop = 0;
  int32_t sourceRegionRight = sourceWidth;
  int32_t sourceRegionBottom = sourceHeight;
  if (overrideTarget && invocation.replaySourceRegion.width > 0 && invocation.replaySourceRegion.height > 0) {
    const auto& region = invocation.replaySourceRegion;
    sourceRegionLeft = std::clamp(region.x, 0, sourceWidth);
    sourceRegionTop = std::clamp(region.y, 0, sourceHeight);
    sourceRegionRight = std::clamp(region.x + region.width, sourceRegionLeft, sourceWidth);
    sourceRegionBottom = std::clamp(region.y + region.height, sourceRegionTop, sourceHeight);
    if (sourceRegionRight == sourceRegionLeft || sourceRegionBottom == sourceRegionTop) {
      sourceRegionLeft = 0;
      sourceRegionTop = 0;
      sourceRegionRight = sourceWidth;
      sourceRegionBottom = sourceHeight;
    }
  }
  const int32_t sourceRegionWidth = sourceRegionRight - sourceRegionLeft;
  const int32_t sourceRegionHeight = sourceRegionBottom - sourceRegionTop;
  const float scaleX = overrideTarget && sourceRegionWidth > 0
                           ? static_cast<float>(targetSize.width) / static_cast<float>(sourceRegionWidth)
                           : 1.0f;
  const float scaleY = overrideTarget && sourceRegionHeight > 0
                           ? static_cast<float>(targetSize.height) / static_cast<float>(sourceRegionHeight)
                           : 1.0f;

  // WebGPU starts a pass scissored to the whole attachment, which is also what a
  // virtual-screen draw wants.
  const std::array<uint32_t, 4> fullTargetScissor{0, 0, targetSize.width, targetSize.height};
  std::array<uint32_t, 4> recordedScissor = fullTargetScissor;
  bool hudScreenScissor = false;
  bool scissorStateKnown = true;
  // A cut native render (render(): only the copies the eyes sample are wanted) draws only where those
  // copies read: every scissor is intersected with this pass's clip.
  const std::array<uint32_t, 4>* passClip =
      invocation.passClips != nullptr && idx < invocation.passClips->size() && (*invocation.passClips)[idx][2] != 0
          ? &(*invocation.passClips)[idx]
          : nullptr;
  const auto apply_scissor = [&pass, passClip](const std::array<uint32_t, 4>& rect) noexcept {
    if (passClip == nullptr) {
      pass.SetScissorRect(rect[0], rect[1], rect[2], rect[3]);
      return;
    }
    const auto& c = *passClip;
    const uint32_t left = std::max(rect[0], c[0]);
    const uint32_t top = std::max(rect[1], c[1]);
    const uint32_t right = std::max(left, std::min(rect[0] + rect[2], c[0] + c[2]));
    const uint32_t bottom = std::max(top, std::min(rect[1] + rect[3], c[1] + c[3]));
    pass.SetScissorRect(left, top, right - left, bottom - top);
  };
  if (passClip != nullptr) {
    apply_scissor(fullTargetScissor);
  }
  Viewport recordedViewport{
      .left = 0.0f,
      .top = 0.0f,
      .width = static_cast<float>(targetSize.width),
      .height = static_cast<float>(targetSize.height),
      .znear = 0.0f,
      .zfar = 1.0f,
  };
  bool hudScreenViewport = false;
  bool viewportStateKnown = true;
  const auto apply_viewport = [&pass, &recordedViewport, &targetSize](bool hudScreen) noexcept {
    pass.SetViewport(hudScreen ? 0.0f : recordedViewport.left, hudScreen ? 0.0f : recordedViewport.top,
                     hudScreen ? static_cast<float>(targetSize.width) : recordedViewport.width,
                     hudScreen ? static_cast<float>(targetSize.height) : recordedViewport.height,
                     recordedViewport.znear, recordedViewport.zfar);
  };
  // KARTFRAME_FPSLOG=2: an eye's pass is cut into timed segments by the kind of draw (gpu_timing_mark).
  const bool profileMarks =
      overrideTarget && invocation.stereoEye < AURORA_STEREO_EYE_COUNT && gpu_timing_marks_enabled();
  int32_t lastMark = -1;
  const auto mark = [&](GpuTimingCategory category) noexcept {
    if (profileMarks && static_cast<int32_t>(category) != lastMark) {
      gpu_timing_mark(pass, category);
      lastMark = static_cast<int32_t>(category);
    }
  };

  for (const auto& cmd : renderPasses[idx].commands) {
#ifdef AURORA_GFX_DEBUG_GROUPS
    {
      size_t firstDiff = lastDebugGroupStack.size();
      for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
        if (i >= cmd.debugGroupStack.size() || cmd.debugGroupStack[i] != lastDebugGroupStack[i]) {
          firstDiff = i;
          break;
        }
      }
      for (size_t i = firstDiff; i < lastDebugGroupStack.size(); ++i) {
        pass.PopDebugGroup();
      }
      for (size_t i = firstDiff; i < cmd.debugGroupStack.size(); ++i) {
        pass.PushDebugGroup(cmd.debugGroupStack[i].c_str());
      }
      lastDebugGroupStack = cmd.debugGroupStack;
    }
#endif
    switch (cmd.type) {
    case CommandType::SetViewport: {
      const auto& vp = cmd.data.setViewport;
      sourceViewport = {vp.left, vp.top, vp.width, vp.height};
      // WebGPU requires 0 <= minDepth <= maxDepth <= 1. vp.znear/vp.zfar are in GX's own distance
      // terms (0 = near); under UseReversedZ the host depth-buffer storage direction is flipped
      // (near = 1, far = 0), so this range has to be remapped through 1-x the same way the
      // projection matrix, depth compare function, and clear value all are - a plain min/max clamp
      // (the previous code here) maps a *restricted* range (e.g. a viewport deliberately narrowed
      // to force something to draw "in front of everything") to the wrong end of the buffer: what
      // should land near the near-storage-extreme (1.0) instead lands near the far-storage-extreme
      // (0.0), so anything else drawn afterward at its true depth wins the compare test and the
      // "in front" geometry silently vanishes. A full [0,1] viewport is unaffected either way,
      // which is why this only broke specific elements, not the whole scene. Matches upstream
      // aurora's apply_viewport (lib/gfx/encoding.cpp).
      //
      // The remapped pair is then ordered and clamped before it reaches WebGPU. For any ordered
      // guest range this is a no-op (znear <= zfar implies 1-zfar <= 1-znear), so upstream's fix is
      // reproduced exactly; it only guards the swapped pair MKW is known to emit, which the raw
      // remap would hand to SetViewport as minDepth > maxDepth and fail validation. The VR eye
      // replay below reuses these recorded values, so the guard covers that path too.
      const float remappedNear = gx::UseReversedZ ? 1.0f - vp.zfar : vp.znear;
      const float remappedFar = gx::UseReversedZ ? 1.0f - vp.znear : vp.zfar;
      const float minDepth = std::clamp(std::min(remappedNear, remappedFar), 0.0f, 1.0f);
      const float maxDepth = std::clamp(std::max(remappedNear, remappedFar), 0.0f, 1.0f);
      recordedViewport = {
          .left = (vp.left - static_cast<float>(sourceRegionLeft)) * scaleX,
          .top = (vp.top - static_cast<float>(sourceRegionTop)) * scaleY,
          .width = vp.width * scaleX,
          .height = vp.height * scaleY,
          .znear = minDepth,
          .zfar = maxDepth,
      };
      hudScreenViewport = false;
      viewportStateKnown = true;
      apply_viewport(false);
    } break;
    case CommandType::SetScissor: {
      const auto& sc = cmd.data.setScissor;
      sourceScissor = {float(sc.x), float(sc.y), float(sc.width), float(sc.height)};
      const auto sourceLeft = std::clamp(sc.x, sourceRegionLeft, sourceRegionRight);
      const auto sourceTop = std::clamp(sc.y, sourceRegionTop, sourceRegionBottom);
      const auto sourceRight = std::clamp(sc.x + sc.width, sourceLeft, sourceRegionRight);
      const auto sourceBottom = std::clamp(sc.y + sc.height, sourceTop, sourceRegionBottom);
      const auto left = static_cast<uint32_t>(
          std::clamp(static_cast<int32_t>(std::floor(static_cast<float>(sourceLeft - sourceRegionLeft) * scaleX)), 0,
                     static_cast<int32_t>(targetSize.width)));
      const auto top = static_cast<uint32_t>(
          std::clamp(static_cast<int32_t>(std::floor(static_cast<float>(sourceTop - sourceRegionTop) * scaleY)), 0,
                     static_cast<int32_t>(targetSize.height)));
      const auto right = static_cast<uint32_t>(
          std::clamp(static_cast<int32_t>(std::ceil(static_cast<float>(sourceRight - sourceRegionLeft) * scaleX)),
                     static_cast<int32_t>(left), static_cast<int32_t>(targetSize.width)));
      const auto bottom = static_cast<uint32_t>(
          std::clamp(static_cast<int32_t>(std::ceil(static_cast<float>(sourceBottom - sourceRegionTop) * scaleY)),
                     static_cast<int32_t>(top), static_cast<int32_t>(targetSize.height)));
      recordedScissor = {left, top, right - left, bottom - top};
      hudScreenScissor = false;
      scissorStateKnown = true;
      apply_scissor(recordedScissor);
    } break;
    case CommandType::Draw: {
      const auto& draw = cmd.data.draw;
      switch (draw.type) {
      case ShaderType::GX: {
        if (overrideTarget && draw.gx.invisible) {
          break;
        }
        if (multiplayer && draw.gx.screenRect &&
            stereo_replay::is_split_screen_furniture(
                *draw.gx.screenRect, {float(display.x), float(display.y), float(display.width), float(display.height)},
                invocation.localPlayerCount)) {
          break;
        }
        if (multiplayer && (!stereo_replay::replay_player_one_draw(sourceViewport, playerRegion,
                                                                   draw.gx.uniformReplayLayout.perspective,
                                                                   draw.gx.uniformReplayLayout.nativeEfbEffect) ||
                            !stereo_replay::subviews_overlap(sourceScissor, playerRegion))) {
          break;
        }
        const gfx::Range* uniformOverride = nullptr;
        // Only a 2D draw the virtual screen actually claimed carries a stereo
        // uniform range without being perspective.
        bool virtualScreenDraw = false;
        if (invocation.stereoEye < draw.gx.stereoUniformRanges.size() &&
            draw.gx.stereoUniformRanges[invocation.stereoEye].size != 0) {
          uniformOverride = &draw.gx.stereoUniformRanges[invocation.stereoEye];
          virtualScreenDraw = !draw.gx.uniformReplayLayout.perspective;
        } else if (invocation.interpolatedFrame >= 0 &&
                   static_cast<size_t>(invocation.interpolatedFrame) < draw.gx.interpolatedUniformRanges.size() &&
                   draw.gx.interpolatedUniformRanges[invocation.interpolatedFrame].size != 0) {
          uniformOverride = &draw.gx.interpolatedUniformRanges[invocation.interpolatedFrame];
        }
        if (overrideTarget && invocation.stereoEye < 2 && draw.gx.uniformReplayLayout.perspective) {
          auto& scene = t_eyeScene;
          if (uniformOverride == nullptr) {
            count_eye_scene(scene.recorded[invocation.stereoEye]);
          } else if (!scene.valid || uniformOverride->offset < scene.fillOffset ||
                     uniformOverride->offset + uniformOverride->size > scene.fillOffset + scene.fillSize) {
            count_eye_scene(scene.outside[invocation.stereoEye]);
          }
        }
        // A virtual-screen draw gets the whole eye (below), so the game's scissor no longer clips
        // it. An element lying wholly outside a scissor the game narrowed would not have shown on
        // the flat frame either (Mario Kart's item roulette sliding up after use): skip it.
        if (virtualScreenDraw && draw.gx.screenRect && recordedScissor != fullTargetScissor &&
            draw.gx.screenRect->width > 0.f && draw.gx.screenRect->height > 0.f &&
            sourceScissor.width > 0.f && sourceScissor.height > 0.f &&
            !stereo_replay::subviews_overlap(*draw.gx.screenRect, sourceScissor)) {
          break;
        }
        // Draw against world depth and mark visible cockpit samples before HUD
        // depth replaces it. The screen pipelines reject those stencil samples.
        if (invocation.cockpitFrame != nullptr && overrideTarget) {
          if (draw.gx.uniformReplayLayout.perspective) {
            *invocation.sceneDrawn = true;
          }
          if (virtualScreenDraw && *invocation.sceneDrawn && !*invocation.cockpitDrawn) {
            mark(GpuTimingCategory::Cockpit);
            cockpit::render(*invocation.cockpitEncoder, *invocation.cockpitFrame, invocation.stereoEye,
                            invocation.cockpitDepth, &pass);
            *invocation.cockpitDrawn = true;
            encodeState = {};
            encodeState.boundTextureBindGroup = gx::g_emptyTextureBindGroup.Get();
            pass.SetBindGroup(0, g_staticBindGroup);
            pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);
            scissorStateKnown = false;
            viewportStateKnown = false;
          }
        }
        // Such a draw no longer lands where the game aimed it, while the
        // recorded scissor still describes the rectangle it occupied on the flat
        // frame (Mario Kart clips the item roulette that way). Honouring that
        // rectangle would cut the reprojected element away, so it gets the whole
        // eye and every other draw gets the game's own rectangle back.
        const bool fullEyeDraw = virtualScreenDraw || (multiplayer && draw.gx.uniformReplayLayout.perspective);
        if (!scissorStateKnown || fullEyeDraw != hudScreenScissor) {
          hudScreenScissor = fullEyeDraw;
          scissorStateKnown = true;
          apply_scissor(fullEyeDraw ? fullTargetScissor : recordedScissor);
        }
        if (!viewportStateKnown || fullEyeDraw != hudScreenViewport) {
          hudScreenViewport = fullEyeDraw;
          viewportStateKnown = true;
          apply_viewport(fullEyeDraw);
        }
        mark(draw.gx.uniformReplayLayout.perspective ? GpuTimingCategory::EyeScene
             : virtualScreenDraw                       ? GpuTimingCategory::EyeHud
             : draw.gx.uniformReplayLayout.nativeEfbEffect ? GpuTimingCategory::EyeEfbEffect
             : draw.gx.screenRect                      ? GpuTimingCategory::EyeFade
                                                       : GpuTimingCategory::EyeOther2D);
        gx::render(draw.gx, pass, encodeState, renderPasses[idx].requireReadyPipelines, uniformOverride,
                   overrideTarget && invocation.target->depthFormat == wgpu::TextureFormat::Depth24PlusStencil8
                       ? (virtualScreenDraw ? draw.gx.stereoScreenPipeline : draw.gx.stereoPipeline)
                       : (virtualScreenDraw ? draw.gx.exactScreenDepthPipeline : 0));
      } break;
      case ShaderType::Clear: {
        mark(GpuTimingCategory::EyeClear);
        auto clearDraw = draw.clear;
        if (overrideTarget && invocation.target->depthFormat == wgpu::TextureFormat::Depth24PlusStencil8) {
          clearDraw.pipeline = clearDraw.stereoPipeline;
        }
        if (multiplayer) {
          const auto& sc = clearDraw.scissor;
          if (clearDraw.copyClear ||
              (clearDraw.useScissor &&
               !stereo_replay::subviews_overlap({float(sc.x), float(sc.y), float(sc.width), float(sc.height)},
                                                playerRegion))) {
            break;
          }
        }
        if (invocation.skipCopyClears && overrideTarget && clearDraw.copyClear && renderPasses[idx].postCopyClear) {
          // The scissored twin of the attachment-load-op case above: the copy's
          // EFB reset, rescaled into eye space, covers the whole eye.
          break;
        }
        if (overrideTarget && clearDraw.useScissor) {
          const auto& sc = clearDraw.scissor;
          const auto sourceLeft = std::clamp(sc.x, sourceRegionLeft, sourceRegionRight);
          const auto sourceTop = std::clamp(sc.y, sourceRegionTop, sourceRegionBottom);
          const auto sourceRight = std::clamp(sc.x + sc.width, sourceLeft, sourceRegionRight);
          const auto sourceBottom = std::clamp(sc.y + sc.height, sourceTop, sourceRegionBottom);
          const auto left =
              std::clamp(static_cast<int32_t>(std::floor(static_cast<float>(sourceLeft - sourceRegionLeft) * scaleX)),
                         0, static_cast<int32_t>(targetSize.width));
          const auto top =
              std::clamp(static_cast<int32_t>(std::floor(static_cast<float>(sourceTop - sourceRegionTop) * scaleY)), 0,
                         static_cast<int32_t>(targetSize.height));
          const auto right =
              std::clamp(static_cast<int32_t>(std::ceil(static_cast<float>(sourceRight - sourceRegionLeft) * scaleX)),
                         left, static_cast<int32_t>(targetSize.width));
          const auto bottom =
              std::clamp(static_cast<int32_t>(std::ceil(static_cast<float>(sourceBottom - sourceRegionTop) * scaleY)),
                         top, static_cast<int32_t>(targetSize.height));
          clearDraw.scissor = ClipRect{
              .x = left,
              .y = top,
              .width = right - left,
              .height = bottom - top,
          };
        }
        // Clear draws set their own viewport and scissor. Stereo replay must use
        // the eye attachment extent here, not the original EFB/desktop extent.
        clear::render(clearDraw, pass, targetSize, encodeState.currentPipeline);
        // The clear helper mutates both pieces of dynamic state without a
        // matching recorded command; force the next GX draw to restore them.
        viewportStateKnown = false;
        scissorStateKnown = false;
      } break;
      }
    } break;
    case CommandType::DebugMarker: {
#if defined(AURORA_GFX_DEBUG_GROUPS)
      pass.InsertDebugMarker(wgpu::StringView(g_debugMarkers[cmd.data.debugMarkerIndex]));
#endif
    } break;
    }
  }

#ifdef AURORA_GFX_DEBUG_GROUPS
  for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
    pass.PopDebugGroup();
  }
#endif
}

bool bind_pipeline(PipelineRef ref, const wgpu::RenderPassEncoder& pass, PipelineRef& currentPipeline,
                   bool requireReady) {
  if (ref == currentPipeline) {
    return true;
  }
  wgpu::RenderPipeline pipeline;
  bool pipelineReady;
  if (!skip_unready_pipelines()) {
    pipelineReady = wait_pipeline(ref, pipeline);
  } else if (requireReady) {
    // The pass resolves into a persistent texture (a one-shot bake such as MKW's minimap), so a
    // skipped draw would never be re-issued. These run behind loads, not mid-race.
    pipelineReady = wait_pipeline_for_persistent_pass(ref, pipeline);
  } else {
    pipelineReady = try_pipeline(ref, pipeline);
  }
  if (!pipelineReady) {
    return false;
  }
  pass.SetPipeline(pipeline);
  currentPipeline = ref;
  return true;
}

static inline Range push(ByteBuffer& target, const uint8_t* data, size_t length, size_t alignment) {
  size_t padding = 0;
  if (alignment != 0) {
    const size_t remainder = length % alignment;
    if (remainder != 0) {
      padding = alignment - remainder;
    }
  }
  auto begin = target.size();
  if (length == 0) {
    length = alignment;
    target.append_zeroes(alignment);
  } else {
    target.append(data, length);
    if (padding > 0) {
      target.append_zeroes(padding);
    }
  }
  return {static_cast<uint32_t>(begin), static_cast<uint32_t>(length + padding)};
}
static inline Range map(ByteBuffer& target, size_t length, size_t alignment) {
  size_t padding = 0;
  if (alignment != 0) {
    const size_t remainder = length % alignment;
    if (remainder != 0) {
      padding = alignment - remainder;
    }
  }
  auto begin = target.size();
  if (length == 0) {
    length = alignment;
    target.append_zeroes(length);
  } else {
    // The caller fills [0, length); only the alignment padding needs clearing,
    // otherwise it hands the next frame's readers whatever was there before.
    target.append_uninitialized(length);
    target.append_zeroes(padding);
  }
  return {static_cast<uint32_t>(begin), static_cast<uint32_t>(length + padding)};
}
Range push_verts(const uint8_t* data, size_t length) { return push(g_verts, data, length, 0); }
Range push_indices(const uint8_t* data, size_t length) { return push(g_indices, data, length, 0); }
Range push_uniform(const uint8_t* data, size_t length) {
  return push(g_uniforms, data, length, g_cachedLimits.minUniformBufferOffsetAlignment);
}
void set_gx_deindex_vertices(bool value) noexcept { g_gxDeindexVertices.store(value, std::memory_order_relaxed); }
bool get_gx_deindex_vertices() noexcept { return g_gxDeindexVertices.load(std::memory_order_relaxed); }
void set_gx_native_vertex_input(bool value) noexcept { g_gxNativeVertexInput.store(value, std::memory_order_relaxed); }
bool get_gx_native_vertex_input() noexcept { return g_gxNativeVertexInput.load(std::memory_order_relaxed); }
Range push_indices32(const uint32_t* data, size_t count) {
  // The 16-bit templates before it may leave the staging on a half word; a 32-bit index must start on a whole one.
  if (const size_t misaligned = g_indices.size() % sizeof(uint32_t); misaligned != 0) {
    g_indices.append_zeroes(sizeof(uint32_t) - misaligned);
  }
  return push(g_indices, reinterpret_cast<const uint8_t*>(data), count * sizeof(uint32_t), 4);
}
size_t vertex_staging_room() noexcept {
  return g_verts.capacity() > g_verts.size() ? g_verts.capacity() - g_verts.size() : 0;
}
size_t index_staging_room() noexcept {
  return g_indices.capacity() > g_indices.size() ? g_indices.capacity() - g_indices.size() : 0;
}
void staging_overflow_abort(size_t requested, size_t capacity) noexcept {
  // The region that overflowed is the one whose use matches its size.
  Log.fatal("Frame staging overflow: {} bytes needed in a mapped region of {} (vertex staging {} of {} KiB in use, "
            "index {} of {} KiB, uniform {} of {} KiB, storage {} of {} KiB)",
            requested, capacity, g_verts.size() >> 10, VertexBufferSize >> 10, g_indices.size() >> 10,
            IndexBufferSize >> 10, g_uniforms.size() >> 10, UniformBufferSize >> 10, g_storage.size() >> 10,
            StorageBufferSize >> 10);
}
Range push_storage(const uint8_t* data, size_t length) {
  return push(g_storage, data, length, g_cachedLimits.minStorageBufferOffsetAlignment);
}
Range push_texture_data(const uint8_t* data, size_t length, u32 bytesPerRow, u32 rowsPerImage) {
  // For CopyBufferToTexture, we need an alignment of 256 per row (see Dawn kTextureBytesPerRowAlignment)
  const auto copyBytesPerRow = AURORA_ALIGN(bytesPerRow, 256);
  const auto range = map(g_textureUpload, copyBytesPerRow * rowsPerImage, 0);
  u8* dst = g_textureUpload.data() + range.offset;
  for (u32 i = 0; i < rowsPerImage; ++i) {
    memcpy(dst, data, bytesPerRow);
    data += bytesPerRow;
    dst += copyBytesPerRow;
  }
  return range;
}
std::pair<ByteBuffer, Range> map_verts(size_t length) {
  const auto range = map(g_verts, length, 4);
  return {ByteBuffer{g_verts.data() + range.offset, range.size}, range};
}
std::pair<ByteBuffer, Range> map_indices(size_t length) {
  const auto range = map(g_indices, length, 4);
  return {ByteBuffer{g_indices.data() + range.offset, range.size}, range};
}
std::pair<ByteBuffer, Range> map_uniform(size_t length) {
  const auto range = map(g_uniforms, length, g_cachedLimits.minUniformBufferOffsetAlignment);
  return {ByteBuffer{g_uniforms.data() + range.offset, range.size}, range};
}
std::pair<ByteBuffer, Range> copy_uniform(Range source) {
  const auto destination = map(g_uniforms, source.size, g_cachedLimits.minUniformBufferOffsetAlignment);
  std::memcpy(g_uniforms.data() + destination.offset, g_uniforms.data() + source.offset, source.size);
  return {ByteBuffer{g_uniforms.data() + destination.offset, destination.size}, destination};
}
std::pair<ByteBuffer, Range> map_storage(size_t length) {
  const auto range = map(g_storage, length, g_cachedLimits.minStorageBufferOffsetAlignment);
  return {ByteBuffer{g_storage.data() + range.offset, range.size}, range};
}

BindGroupRef bind_group_ref(const WGPUBindGroupDescriptor& descriptor) {
  const auto id = xxh3_hash(descriptor);
  const auto it = g_cachedBindGroups.find(id);
  if (it == g_cachedBindGroups.end()) {
    auto bg = wgpu::BindGroup::Acquire(wgpuDeviceCreateBindGroup(g_device.Get(), &descriptor));
    g_cachedBindGroups.emplace(id, CachedBindGroup{
                                       .bindGroup = std::move(bg),
                                       .lastUsedFrame = g_frameIndex,
                                   });
  } else {
    it->second.lastUsedFrame = g_frameIndex;
  }
  return id;
}

wgpu::BindGroup& find_bind_group(BindGroupRef id) {
  const auto it = g_cachedBindGroups.find(id);
  CHECK(it != g_cachedBindGroups.end(), "get_bind_group: failed to locate {:x}", id);
  return it->second.bindGroup;
}

wgpu::Sampler& sampler_ref(const wgpu::SamplerDescriptor& descriptor) {
  const auto id = xxh3_hash(descriptor);
  auto it = g_cachedSamplers.find(id);
  if (it == g_cachedSamplers.end()) {
    it = g_cachedSamplers.try_emplace(id, g_device.CreateSampler(&descriptor)).first;
  }
  return it->second;
}

uint32_t align_uniform(uint32_t value) { return AURORA_ALIGN(value, g_cachedLimits.minUniformBufferOffsetAlignment); }

void insert_debug_marker(std::string label) {
#if defined(AURORA_GFX_DEBUG_GROUPS)
  auto idx = g_debugMarkers.size();
  g_debugMarkers.emplace_back(std::move(label));
  push_command(CommandType::DebugMarker, {.debugMarkerIndex = idx});
#endif
}

} // namespace aurora::gfx

void aurora::gfx::push_debug_group(std::string label) {
#if defined(AURORA_GFX_DEBUG_GROUPS)
  g_debugGroupStack.push_back(std::move(label));
#endif
}
void aurora_push_debug_group(const char* label) {
#ifdef AURORA_GFX_DEBUG_GROUPS
  aurora::gfx::g_debugGroupStack.emplace_back(label);
#endif
}
void aurora_pop_debug_group() {
#ifdef AURORA_GFX_DEBUG_GROUPS
  if (aurora::gfx::g_debugGroupStack.empty()) {
    aurora::gfx::Log.error("Debug group stack underflowed!");
    return;
  }

  aurora::gfx::g_debugGroupStack.pop_back();
#endif
}

const AuroraStats* aurora_get_stats() { return &aurora::gfx::g_stats; }

void aurora_set_vr_hand_mesh(uint32_t hand, const AuroraVRHandVertex* vertices, uint32_t vertexCount,
                             const uint16_t* indices, uint32_t indexCount, const float* bindPoses,
                             const int32_t* parents, uint32_t jointCount) {
  using namespace aurora::gfx::cockpit;
  if (hand >= 2) {
    return;
  }
  std::shared_ptr<HandMesh> mesh;
  if (vertices && indices && bindPoses && parents && jointCount == 26 && vertexCount > 0 && vertexCount <= 65535 &&
      indexCount <= 100000 && indexCount % 3 == 0) {
    for (uint32_t i = 0; i < indexCount; ++i) {
      if (indices[i] >= vertexCount) {
        return;
      }
    }
    mesh = std::make_shared<HandMesh>();
    mesh->vertices.assign(vertices, vertices + vertexCount);
    mesh->indices.assign(indices, indices + indexCount);
    for (int j = 0; j < 26; ++j) {
      mesh->bind[j] = from_pose(bindPoses + j * 7);
      mesh->inverseBind[j] = inverse(mesh->bind[j]);
      mesh->parents[j] = parents[j];
    }
  }
  std::lock_guard lock(meshMutex);
  meshes[hand] = std::move(mesh);
  ++meshRevision;
}
