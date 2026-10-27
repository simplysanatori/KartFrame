// SPDX-License-Identifier: GPL-3.0-or-later
#include <aurora/vulkan_win32_interop.h>
#include "../internal.hpp"
#include "../stereo.hpp"
#include "../stereo_overlay.hpp"
#include "gpu.hpp"
#if (defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))) && defined(WEBGPU_DAWN) && defined(DAWN_ENABLE_BACKEND_VULKAN)
#if defined(_WIN32)
#include <windows.h>
#else
// The patched Dawn is linked statically on Linux; its Aurora Vulkan ABI is resolved at link time.
extern "C" {
uint32_t AuroraDawnVulkanVersion();
int AuroraDawnVulkanConfigure(const AuroraDawnVulkanHooks* hooks);
int AuroraDawnVulkanGetHandles(void* handle, AuroraDawnVulkanHandles* out);
void* AuroraDawnVulkanWrap(void* handle, const void* desc, uint64_t image);
int AuroraDawnVulkanRelease(void* handle, void* const* textures, uint32_t count);
void* AuroraDawnVulkanLock(void* handle);
void AuroraDawnVulkanUnlock(void* guard);
int AuroraDawnVulkanDrain(void* handle);
}
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>
namespace aurora::vulkan_win32 {
namespace {
Module Log("aurora::vulkan_win32");
// KARTFRAME_FRAME_STAMP=1 (tests only): each eye image gets a 64x15 pixel block in its top left
// corner naming the display time its poses were located for and the eye, written after the eye
// copy. The mock runtime (MOCK_XR_STAMP) reads it back from the image a projection layer submits
// and compares it with the locate the layer's poses came from. Layout, one pixel a cell, white = 1:
// row 0 the 64 bits of the display time (steady clock ns, bit 0 first), row 1 their complement,
// row 2: cell 0 the eye (1 = right), cell 1 its complement, cells 2-5 the marker 1 0 1 0.
// Lane L, the scene state this eye's draws used (stereo::EyeScene), rows 3 and 5 two 64-bit words, rows 4
// and 6 their complements:
// row 3: bits 0-15 weight x 65535, 16-31 previous game frame (low 16 bits), 32-47 current game frame,
//        48 valid, 49 late (filled at the set's sample time), 50 anchored, 51 continuous, 56-63 set (low 8);
// row 5: perspective draws filled from the pair (bits 0-15), filled current-only (16-31), drawn with the
//        recorded uniform (32-47), drawn with a range outside this set's fill (48-63).
// Lane P, the head pose this eye's uniform fill wrote (stereo::EyeScene::pose*), rows 7, 9 and 11 three
// 64-bit words, rows 8, 10 and 12 their complements:
// row 7: the display time (steady ns) of the packet whose eye transform the fill wrote (0: none);
// row 9: the eye's orientation recovered from that transform, x, y, z, w as signed 16-bit (x 32767);
// row 11: bits 0-15 draws of this eye filled with another packet or transform, 16-47 the packet's token
//         (low 32 bits), 48 a pose was recorded;
// row 13: the tangents of that fill's eye frustum, left, right, down, up as signed 16-bit (x 8192); row 14
//         its complement.
// Lane P: one pixel a cell (the block is 64x15 pixels; it was 4x4-pixel cells, 256x28, before): the mock
// reads the image raw, and the smaller block keeps the test stamp's cost out of the frame's timing.
constexpr uint32_t kStampCell = 1;
constexpr uint32_t kStampRows = 15;
std::array<uint64_t, 2> SceneWords(const stereo::EyeScene& scene) {
  if (!scene.valid) return {0, 0};
  const uint64_t weight = static_cast<uint64_t>(std::lround(std::clamp(scene.weight, 0.f, 1.f) * 65535.f));
  const uint64_t a = weight | (static_cast<uint64_t>(scene.previousFrame & 0xffffu) << 16) |
                     (static_cast<uint64_t>(scene.currentFrame & 0xffffu) << 32) | (1ull << 48) |
                     (scene.late ? 1ull << 49 : 0) | (scene.anchored ? 1ull << 50 : 0) |
                     (scene.continuous ? 1ull << 51 : 0) | (static_cast<uint64_t>(scene.set & 0xffu) << 56);
  const uint64_t b = static_cast<uint64_t>(scene.interpolated) | (static_cast<uint64_t>(scene.current) << 16) |
                     (static_cast<uint64_t>(scene.recorded) << 32) | (static_cast<uint64_t>(scene.outside) << 48);
  return {a, b};
}
std::array<uint64_t, 4> PoseWords(const stereo::EyeScene& scene) {
  if (!scene.valid || scene.poseTime == 0) return {0, 0, 0, 0};
  uint64_t q = 0;
  for (uint32_t k = 0; k < 4; ++k) {
    const auto v = static_cast<int16_t>(std::lround(std::clamp(scene.poseOrientation[k], -1.f, 1.f) * 32767.f));
    q |= static_cast<uint64_t>(static_cast<uint16_t>(v)) << (16 * k);
  }
  const uint64_t e = static_cast<uint64_t>(scene.poseMixed) | ((scene.poseToken & 0xffffffffull) << 16) | (1ull << 48);
  uint64_t t = 0;
  for (uint32_t k = 0; k < 4; ++k) {
    const auto v = static_cast<int16_t>(std::lround(std::clamp(scene.poseTangents[k], -3.99f, 3.99f) * 8192.f));
    t |= static_cast<uint64_t>(static_cast<uint16_t>(v)) << (16 * k);
  }
  return {scene.poseTime, q, e, t};
}
bool StampEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("KARTFRAME_FRAME_STAMP");
    return env != nullptr && env[0] == '1';
  }();
  return enabled;
}
// The whole block is built on the CPU and written into a small texture per eye (queue.WriteTexture runs
// before this set's submission and after the last one's, so reusing it per eye is safe), then copied into
// the eye image with one copy. One pixel a bit (lane P; 4x4 cells before): 64x15 pixels, 3.8 KB an eye.
void WriteStamp(wgpu::CommandEncoder& encoder, const wgpu::Texture& target, wgpu::TextureFormat format, uint64_t value,
                uint32_t eye, const stereo::EyeScene& scene, uint32_t x = 0) {
  constexpr uint32_t kWidth = 64 * kStampCell, kHeight = kStampRows * kStampCell;
  static std::array<wgpu::Texture, 2> blocks;
  static std::array<wgpu::TextureFormat, 2> blockFormats{};
  const uint32_t slot = eye == 1 ? 1 : 0;
  const wgpu::Extent3D size{kWidth, kHeight, 1};
  if (!blocks[slot] || blockFormats[slot] != format) {
    const wgpu::TextureDescriptor descriptor{.label = "Frame stamp block",
                                             .usage = wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst,
                                             .dimension = wgpu::TextureDimension::e2D,
                                             .size = size,
                                             .format = format,
                                             .mipLevelCount = 1,
                                             .sampleCount = 1};
    blocks[slot] = webgpu::g_device.CreateTexture(&descriptor);
    blockFormats[slot] = format;
  }
  std::array<uint8_t, size_t{kWidth} * kHeight * 4> pixels;
  const auto cell = [&](uint32_t column, uint32_t row, bool one) {
    for (uint32_t y = 0; y < kStampCell; ++y)
      for (uint32_t x = 0; x < kStampCell; ++x) {
        auto* p = &pixels[((row * kStampCell + y) * kWidth + column * kStampCell + x) * 4];
        p[0] = p[1] = p[2] = one ? 255 : 0;
        p[3] = 255;
      }
  };
  const auto word = [&](uint32_t row, uint64_t bits) {
    for (uint32_t bit = 0; bit < 64; ++bit) {
      const bool one = ((bits >> bit) & 1u) != 0;
      cell(bit, row, one);
      cell(bit, row + 1, !one);
    }
  };
  word(0, value);
  for (uint32_t column = 0; column < 64; ++column)
    cell(column, 2, column == 0 ? eye == 1 : column == 1 ? eye != 1 : column < 6 ? ((column - 2) & 1u) == 0 : false);
  const auto words = SceneWords(scene);
  word(3, words[0]);
  word(5, words[1]);
  const auto pose = PoseWords(scene);
  for (uint32_t k = 0; k < pose.size(); ++k) word(7 + 2 * k, pose[k]);
  const wgpu::TexelCopyTextureInfo block{.texture = blocks[slot]};
  const wgpu::TexelCopyBufferLayout layout{.bytesPerRow = kWidth * 4, .rowsPerImage = kHeight};
  webgpu::g_queue.WriteTexture(&block, pixels.data(), pixels.size(), &layout, &size);
  const wgpu::TexelCopyTextureInfo destination{.texture = target, .origin = {x, 0, 0}};
  encoder.CopyTextureToTexture(&block, &destination, &size);
}
// Lane AA, KARTFRAME_STEREO_PACKED=1 (test only, read by the OpenXR backend as well): a set's eyes share one
// swapchain image, eye N in the half at x = N * eye width. The backend then hands the same image as both eye
// targets (twice the eye's width), and in the menus the screen's target is that image too (its left half).
bool PackedSetting() {
  static const bool packed = [] {
    const char* env = std::getenv("KARTFRAME_STEREO_PACKED");
    return env != nullptr && env[0] == '1';
  }();
  return packed;
}
// Set on the frame worker around the submitted callback (see aurora_vulkan_win32_submitting_retained).
thread_local bool submittingRetained = false;
struct Api {
  AuroraDawnVulkanConfigureFn configure = nullptr;
  AuroraDawnVulkanHandlesFn handles = nullptr;
  AuroraDawnVulkanWrapFn wrap = nullptr;
  AuroraDawnVulkanReleaseFn release = nullptr;
  AuroraDawnVulkanLockFn lock = nullptr;
  AuroraDawnVulkanUnlockFn unlock = nullptr;
  AuroraDawnVulkanDrainFn drain = nullptr;
  bool Load() {
#if defined(_WIN32)
    HMODULE module = GetModuleHandleW(L"webgpu_dawn.dll");
    if (!module) return false;
    auto version = reinterpret_cast<AuroraDawnVulkanVersionFn>(GetProcAddress(module, "AuroraDawnVulkanVersion"));
    if (!version || version() != AURORA_DAWN_VULKAN_ABI) return false;
#define LOAD(member, type, name) member = reinterpret_cast<type>(GetProcAddress(module, name)); if (!member) return false
    LOAD(configure, AuroraDawnVulkanConfigureFn, "AuroraDawnVulkanConfigure");
    LOAD(handles, AuroraDawnVulkanHandlesFn, "AuroraDawnVulkanGetHandles");
    LOAD(wrap, AuroraDawnVulkanWrapFn, "AuroraDawnVulkanWrap");
    LOAD(release, AuroraDawnVulkanReleaseFn, "AuroraDawnVulkanRelease");
    LOAD(lock, AuroraDawnVulkanLockFn, "AuroraDawnVulkanLock");
    LOAD(unlock, AuroraDawnVulkanUnlockFn, "AuroraDawnVulkanUnlock");
    LOAD(drain, AuroraDawnVulkanDrainFn, "AuroraDawnVulkanDrain");
#undef LOAD
    return true;
#else
    if (AuroraDawnVulkanVersion() != AURORA_DAWN_VULKAN_ABI) return false;
    configure = &AuroraDawnVulkanConfigure;
    handles = &AuroraDawnVulkanGetHandles;
    wrap = &AuroraDawnVulkanWrap;
    release = &AuroraDawnVulkanRelease;
    lock = &AuroraDawnVulkanLock;
    unlock = &AuroraDawnVulkanUnlock;
    drain = &AuroraDawnVulkanDrain;
    return true;
#endif
  }
} api;
int64_t VkFormat(wgpu::TextureFormat format) {
  switch (format) {
  case wgpu::TextureFormat::RGBA8Unorm: return 37;
  case wgpu::TextureFormat::RGBA8UnormSrgb: return 43;
  case wgpu::TextureFormat::BGRA8Unorm: return 44;
  case wgpu::TextureFormat::BGRA8UnormSrgb: return 50;
  case wgpu::TextureFormat::RGBA16Float: return 97;
  default: return 0;
  }
}
bool CopyCompatible(int64_t a, int64_t b) {
  return a == b || ((a == 37 || a == 43) && (b == 37 || b == 43)) ||
                  ((a == 44 || a == 50) && (b == 44 || b == 50));
}
struct Import { uint64_t image; uint32_t width, height; wgpu::TextureFormat format; wgpu::Texture texture; };
// The eyes, then the settings panel's layer image after them.
constexpr uint32_t kMaxImages = 3;
class Bridge {
public:
  std::mutex mutex;
  std::vector<Import> imports;
  std::array<AuroraD3D12StereoTarget, kMaxImages> targets{};
  std::array<wgpu::Texture, kMaxImages> active{};
  uint64_t token = 0;
  uint32_t count = 0;
  // Images this frame copies: `count` eyes, plus the panel when `panel` is set.
  uint32_t images = 0;
  bool panel = false;
  bool encoded = false;
  // The eyes render straight into this set's images (DirectTargets): it can no longer be canceled.
  bool claimed = false;
  AuroraD3D12StereoSubmittedCallback callback;
  void* userdata;
  Bridge(AuroraD3D12StereoSubmittedCallback cb, void* data) : callback(cb), userdata(data) {}
  bool Set(uint64_t next, const AuroraD3D12StereoTarget* data, uint32_t n, const AuroraD3D12StereoTarget* panelTarget) {
    if (!next || !data || !n || n > 2) return false;
    std::lock_guard guard(mutex);
    if (token) return false;
    const auto valid = [](const AuroraD3D12StereoTarget& target) {
      return target.resource && target.width && target.height;
    };
    for (uint32_t i = 0; i < n; ++i) {
      if (!valid(data[i])) return false;
    }
    if (panelTarget && !valid(*panelTarget)) return false;
    for (uint32_t i = 0; i < n; ++i) targets[i] = data[i];
    panel = panelTarget != nullptr;
    if (panel) targets[n] = *panelTarget;
    token = next; count = n; images = n + (panel ? 1u : 0u); return true;
  }
  bool Cancel(uint64_t wanted) {
    std::unique_lock guard(mutex, std::try_to_lock);
    if (!guard.owns_lock() || encoded || claimed || !token || token != wanted) return false;
    token = 0; return true;
  }
  // How the eyes reach the swapchain images (lane I). Default srgb: a full-screen draw that decodes the
  // eye's sRGB-encoded bytes and writes them through an sRGB view of the sRGB image (the same bytes; the
  // stamps read back exactly). The image then needs no MUTABLE_FORMAT, so the driver may compress it
  // (Turnip: UBWC), and the eyes' way into it cost 0.47 ms a set instead of the copy's 2.3 ms (mock, GP
  // race, compositor running; RACE120.md). KARTFRAME_SINK=copy is the old texture copy into a UNORM view
  // of a MUTABLE_FORMAT image, =blit the same through a full-screen draw (the backend asks for
  // MUTABLE_FORMAT for both). Not for a UNORM swapchain (no sRGB format offered): that takes the copy.
  enum class Sink { Copy, Blit, Srgb };
  static Sink SinkMode() {
    static const Sink mode = [] {
      const char* env = std::getenv("KARTFRAME_SINK");
      const std::string_view v = env != nullptr ? env : "";
      return v == "blit" ? Sink::Blit : v == "copy" ? Sink::Copy : Sink::Srgb;
    }();
    return mode;
  }
  static bool SinkBlit() { return SinkMode() != Sink::Copy; }
  // The sRGB format of an sRGB swapchain image (VkFormat 43 or 50), for the draw that writes it.
  static wgpu::TextureFormat TargetSrgb(int64_t vk) {
    return vk == 43 ? wgpu::TextureFormat::RGBA8UnormSrgb : vk == 50 ? wgpu::TextureFormat::BGRA8UnormSrgb
                                                         : wgpu::TextureFormat::Undefined;
  }
  static wgpu::TextureFormat SrgbOf(wgpu::TextureFormat f) {
    return f == wgpu::TextureFormat::BGRA8Unorm ? wgpu::TextureFormat::BGRA8UnormSrgb
         : f == wgpu::TextureFormat::RGBA8Unorm ? wgpu::TextureFormat::RGBA8UnormSrgb
                                                : f;
  }
  // [1]: the packed variant (KARTFRAME_STEREO_PACKED), whose right eye's fragments sit one eye width to the right.
  std::array<wgpu::RenderPipeline, 2> srgbPipelines;
  std::array<wgpu::TextureFormat, 2> srgbPipelineFormats{wgpu::TextureFormat::Undefined, wgpu::TextureFormat::Undefined};
  const wgpu::RenderPipeline& SrgbPipeline(wgpu::TextureFormat format, bool packed = false) {
    auto& srgbPipeline = srgbPipelines[packed ? 1 : 0];
    auto& srgbPipelineFormat = srgbPipelineFormats[packed ? 1 : 0];
    if (srgbPipeline && srgbPipelineFormat == format) return srgbPipeline;
    wgpu::ShaderSourceWGSL code{};
    code.code = R"(
      @group(0) @binding(0) var s: sampler;
      @group(0) @binding(1) var t: texture_2d<f32>;
      @vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
        let p = array(vec2f(-1.0, 1.0), vec2f(-1.0, -3.0), vec2f(3.0, 1.0));
        return vec4f(p[i], 0.0, 1.0);
      }
      fn decode(c: f32) -> f32 {
        return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045);
      }
      @fragment fn fs(@builtin(position) p: vec4f) -> @location(0) vec4f {
        let c = textureLoad(t, vec2i(p.xy), 0);
        return vec4f(decode(c.r), decode(c.g), decode(c.b), 1.0);
      }
    )";
    if (packed) {
      code.code = R"(
      @group(0) @binding(0) var s: sampler;
      @group(0) @binding(1) var t: texture_2d<f32>;
      @vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
        let p = array(vec2f(-1.0, 1.0), vec2f(-1.0, -3.0), vec2f(3.0, 1.0));
        return vec4f(p[i], 0.0, 1.0);
      }
      fn decode(c: f32) -> f32 {
        return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045);
      }
      @fragment fn fs(@builtin(position) p: vec4f) -> @location(0) vec4f {
        var q = vec2i(p.xy);
        let w = i32(textureDimensions(t).x);
        if (q.x >= w) { q.x -= w; }
        let c = textureLoad(t, q, 0);
        return vec4f(decode(c.r), decode(c.g), decode(c.b), 1.0);
      }
    )";
    }
    wgpu::ShaderModuleDescriptor md{};
    md.nextInChain = &code;
    const auto module = webgpu::g_device.CreateShaderModule(&md);
    const auto layout = webgpu::g_CopyPipeline.GetBindGroupLayout(0);
    const wgpu::PipelineLayoutDescriptor ld{.bindGroupLayoutCount = 1, .bindGroupLayouts = &layout};
    const wgpu::ColorTargetState target{.format = format};
    const wgpu::FragmentState fs{.module = module, .entryPoint = "fs", .targetCount = 1, .targets = &target};
    wgpu::RenderPipelineDescriptor pd{};
    pd.layout = webgpu::g_device.CreatePipelineLayout(&ld);
    pd.vertex = {.module = module, .entryPoint = "vs"};
    pd.fragment = &fs;
    srgbPipeline = webgpu::g_device.CreateRenderPipeline(&pd);
    srgbPipelineFormat = format;
    return srgbPipeline;
  }
  std::vector<std::pair<WGPUTextureView, wgpu::BindGroup>> blitSources;
  std::vector<std::pair<WGPUTexture, wgpu::TextureView>> blitTargets;
  wgpu::Sampler blitSampler;
  // The bind group reading `source` (cached).
  const wgpu::BindGroup& BlitSource(const wgpu::TextureView& source) {
    if (!blitSampler) {
      const wgpu::SamplerDescriptor sd{.magFilter = wgpu::FilterMode::Nearest, .minFilter = wgpu::FilterMode::Nearest};
      blitSampler = webgpu::g_device.CreateSampler(&sd);
    }
    auto src = std::find_if(blitSources.begin(), blitSources.end(), [&](const auto& e) { return e.first == source.Get(); });
    if (src == blitSources.end()) {
      if (blitSources.size() >= 8) blitSources.erase(blitSources.begin());
      blitSources.emplace_back(source.Get(), webgpu::create_copy_bind_group(source, blitSampler));
      src = blitSources.end() - 1;
    }
    return src->second;
  }
  // KARTFRAME_STEREO_PACKED: `n` eyes into the halves of one packed image (eye k at x = k * eye width), one
  // render pass, one draw per eye in its own viewport.
  void BlitPacked(wgpu::CommandEncoder& encoder, const wgpu::TextureView* const* sources, uint32_t n,
                  uint32_t eyeWidth, uint32_t height, const wgpu::Texture& target) {
    std::array<wgpu::BindGroup, 2> groups;
    for (uint32_t k = 0; k < n; ++k) groups[k] = BlitSource(*sources[k]);
    auto dst = std::find_if(blitTargets.begin(), blitTargets.end(), [&](const auto& e) { return e.first == target.Get(); });
    if (dst == blitTargets.end()) {
      if (blitTargets.size() >= 16) blitTargets.erase(blitTargets.begin());
      blitTargets.emplace_back(target.Get(), target.CreateView());
      dst = blitTargets.end() - 1;
    }
    const wgpu::RenderPassColorAttachment ca{.view = dst->second, .loadOp = wgpu::LoadOp::Clear,
                                             .storeOp = wgpu::StoreOp::Store};
    const wgpu::RenderPassDescriptor rd{.label = "VR eyes to packed swapchain", .colorAttachmentCount = 1,
                                        .colorAttachments = &ca};
    auto pass = encoder.BeginRenderPass(&rd);
    pass.SetPipeline(SinkMode() == Sink::Srgb ? SrgbPipeline(target.GetFormat(), true) : webgpu::g_CopyPipeline);
    for (uint32_t k = 0; k < n; ++k) {
      pass.SetViewport(static_cast<float>(k * eyeWidth), 0.f, static_cast<float>(eyeWidth), static_cast<float>(height),
                       0.f, 1.f);
      pass.SetScissorRect(k * eyeWidth, 0, eyeWidth, height);
      pass.SetBindGroup(0, groups[k], 0, nullptr);
      pass.Draw(3);
    }
    pass.End();
  }
  void Blit(wgpu::CommandEncoder& encoder, const wgpu::TextureView& source, const wgpu::Texture& target) {
    if (!blitSampler) {
      const wgpu::SamplerDescriptor sd{.magFilter = wgpu::FilterMode::Nearest, .minFilter = wgpu::FilterMode::Nearest};
      blitSampler = webgpu::g_device.CreateSampler(&sd);
    }
    auto src = std::find_if(blitSources.begin(), blitSources.end(), [&](const auto& e) { return e.first == source.Get(); });
    if (src == blitSources.end()) {
      if (blitSources.size() >= 8) blitSources.erase(blitSources.begin());
      blitSources.emplace_back(source.Get(), webgpu::create_copy_bind_group(source, blitSampler));
      src = blitSources.end() - 1;
    }
    auto dst = std::find_if(blitTargets.begin(), blitTargets.end(), [&](const auto& e) { return e.first == target.Get(); });
    if (dst == blitTargets.end()) {
      if (blitTargets.size() >= 16) blitTargets.erase(blitTargets.begin());
      blitTargets.emplace_back(target.Get(), target.CreateView());
      dst = blitTargets.end() - 1;
    }
    const wgpu::RenderPassColorAttachment ca{.view = dst->second, .loadOp = wgpu::LoadOp::Clear,
                                             .storeOp = wgpu::StoreOp::Store};
    const wgpu::RenderPassDescriptor rd{.label = "VR eye to swapchain", .colorAttachmentCount = 1, .colorAttachments = &ca};
    auto pass = encoder.BeginRenderPass(&rd);
    pass.SetPipeline(SinkMode() == Sink::Srgb ? SrgbPipeline(target.GetFormat()) : webgpu::g_CopyPipeline);
    pass.SetBindGroup(0, src->second, 0, nullptr);
    pass.Draw(3);
    pass.End();
  }
  // The wrapped texture of target i as `format`, imported on first use (mutex held).
  wgpu::Texture* ImportTarget(uint32_t i, wgpu::TextureFormat format) {
    const auto& target = targets[i];
    const uint64_t image = reinterpret_cast<uintptr_t>(target.resource);
    auto it = std::find_if(imports.begin(), imports.end(), [&](const Import& entry) { return entry.image == image; });
    if (it != imports.end() && (it->width != target.width || it->height != target.height || it->format != format)) {
      // The runtime reuses VkImage handles across swapchain recreation, and Aurora's eye format can
      // change with the surface: re-wrap.
      imports.erase(it);
      it = imports.end();
    }
    if (it == imports.end()) {
      wgpu::TextureDescriptor descriptor;
      descriptor.usage = wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::RenderAttachment;
      descriptor.size = {target.width, target.height, 1};
      descriptor.format = format;
      void* wrapped = api.wrap(webgpu::g_device.Get(), &descriptor, image);
      if (!wrapped) return nullptr;
      imports.push_back({image, target.width, target.height, format,
                        wgpu::Texture::Acquire(static_cast<WGPUTexture>(wrapped))});
      it = imports.end() - 1;
    }
    return &it->texture;
  }
  bool DirectTargets(uint64_t wanted, wgpu::TextureFormat format, uint32_t width, uint32_t height,
                     std::array<wgpu::Texture, AURORA_STEREO_EYE_COUNT>& out) {
    std::lock_guard guard(mutex);
    if (!token || token != wanted || encoded || count != AURORA_STEREO_EYE_COUNT || SinkMode() == Sink::Srgb) return false;
    for (uint32_t i = 0; i < count; ++i) {
      if (targets[i].width != width || targets[i].height != height ||
          !CopyCompatible(VkFormat(format), targets[i].dxgiFormat)) return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      auto* texture = ImportTarget(i, format);
      if (!texture) return false;
      out[i] = *texture;
    }
    claimed = true;
    return true;
  }
  bool Encode(wgpu::CommandEncoder& encoder, const stereo::SinkFrame& frame) {
    std::lock_guard guard(mutex);
    if (!token || encoded || frame.frameToken != token) return false;
    std::array<stereo::EyeImage, kMaxImages> sources{};
    for (uint32_t i = 0; i < count; ++i) sources[i] = frame.eyes[i];
    if (panel && !stereo_overlay::layer_source(encoder, targets[count].width, targets[count].height, sources[count]))
      return false;
    // KARTFRAME_STEREO_PACKED: the x of eye i's half in its target (an eye half as wide as its target), -1
    // when the eye fills its target.
    const auto region = [&](uint32_t i) -> int32_t {
      if (!PackedSetting() || i >= count || sources[i].size.width * 2 != targets[i].width ||
          sources[i].size.height != targets[i].height)
        return -1;
      return i == 1 && targets[1].resource == targets[0].resource ? static_cast<int32_t>(sources[i].size.width) : 0;
    };
    // Validate/import every target before recording any copy.
    for (uint32_t i = 0; i < images; ++i) {
      const auto& eye = sources[i];
      const auto& target = targets[i];
      if (!eye.texture || ((eye.size.width != target.width || eye.size.height != target.height) && region(i) < 0))
        return false;
      // Lane O: an eye swapchain in the other channel order (R8G8B8A8_SRGB against Aurora's BGRA, as
      // GalaxyFrame's) can only be written by the sRGB draw, which writes any order; that includes the
      // menus' screen image (the left image of a pair), which the copy wrote before.
      const bool swizzle = !CopyCompatible(VkFormat(eye.format), target.dxgiFormat);
      const bool srgbTarget = TargetSrgb(target.dxgiFormat) != wgpu::TextureFormat::Undefined;
      const bool srgbEye = SinkMode() == Sink::Srgb && srgbTarget && SrgbOf(eye.format) != eye.format &&
                           ((i < count && frame.mode == AURORA_STEREO_FRAME_IMMERSIVE_REPLAY) || swizzle);
      if (swizzle && !(srgbEye && eye.view != nullptr && *eye.view)) return false;
      auto* texture = ImportTarget(i, srgbEye ? TargetSrgb(target.dxgiFormat) : eye.format);
      if (!texture) return false;
      active[i] = *texture;
    }
    const auto blits = [&](uint32_t i) {
      return SinkBlit() && sources[i].view != nullptr && *sources[i].view &&
             (SinkMode() == Sink::Blit || active[i].GetFormat() != sources[i].format);
    };
    // Both eyes in one packed image: one pass draws both halves (the copies take each half on their own).
    const bool packedPair = count == 2 && region(0) == 0 && region(1) > 0 && blits(0) && blits(1);
    for (uint32_t i = 0; i < images; ++i) {
      wgpu::TexelCopyTextureInfo source, destination;
      source.texture = *sources[i].texture;
      destination.texture = active[i];
      const int32_t x = region(i);
      wgpu::Extent3D size{targets[i].width, targets[i].height, 1};
      if (x >= 0) {
        destination.origin = {static_cast<uint32_t>(x), 0, 0};
        size.width = sources[i].size.width;
      }
      // An eye rendered straight into its swapchain image (DirectTargets) needs no copy.
      if (source.texture.Get() == destination.texture.Get()) {
      } else if (packedPair) {
        if (i == 0) {
          const wgpu::TextureView* views[2] = {sources[0].view, sources[1].view};
          BlitPacked(encoder, views, 2, sources[0].size.width, targets[0].height, active[0]);
        }
      } else if (x >= 0 && blits(i)) {
        const wgpu::TextureView* views[1] = {sources[i].view};
        BlitPacked(encoder, views, 1, sources[i].size.width, targets[i].height, active[i]);
      } else if (blits(i)) {
        Blit(encoder, *sources[i].view, active[i]);
      } else {
        encoder.CopyTextureToTexture(&source, &destination, &size);
      }
      if (StampEnabled() && frame.mode == AURORA_STEREO_FRAME_IMMERSIVE_REPLAY && i < count && count == 2 &&
          size.width >= 64 * kStampCell &&
          targets[i].height >= kStampRows * kStampCell)
        WriteStamp(encoder, active[i], active[i].GetFormat(), frame.displayTimeNanos, i, frame.scene[i],
                   x >= 0 ? static_cast<uint32_t>(x) : 0);
    }
    encoded = true;
    return true;
  }
  void Submitted(const stereo::SinkFrame& frame) {
    std::lock_guard guard(mutex);
    if (!token || !encoded || token != frame.frameToken) return;
    std::array<void*, kMaxImages> textures{};
    uint32_t released = 0;
    // A packed image (KARTFRAME_STEREO_PACKED) is both eyes' target: its barrier once.
    for (uint32_t i = 0; i < images; ++i)
      if (std::find(textures.begin(), textures.begin() + released, active[i].Get()) == textures.begin() + released)
        textures[released++] = active[i].Get();
    // Append the COLOR_ATTACHMENT_OPTIMAL release barriers to Dawn's queue,
    // flush them under its device guard, then allow the XR thread to release.
    const bool success = api.release(webgpu::g_device.Get(), textures.data(), released) != 0;
    const auto completed = token;
    token = 0; encoded = false; claimed = false;
    submittingRetained = frame.retained;
    callback(completed, success, userdata);
    submittingRetained = false;
  }
};
std::unique_ptr<Bridge> bridge;
}
}
bool aurora_vulkan_win32_configure(const AuroraDawnVulkanHooks* hooks) {
  using namespace aurora::vulkan_win32;
  return api.Load() && api.configure(hooks);
}
bool aurora_vulkan_win32_get_handles(AuroraDawnVulkanHandles* handles, int64_t* format) {
  using namespace aurora;
  if (!webgpu::g_device || webgpu::g_backendType != wgpu::BackendType::Vulkan || !vulkan_win32::api.Load()) return false;
  *format = vulkan_win32::VkFormat(webgpu::g_graphicsConfig.surfaceConfiguration.format);
  return *format && vulkan_win32::api.handles(webgpu::g_device.Get(), handles);
}
bool aurora_vulkan_win32_enable(AuroraD3D12StereoSubmittedCallback cb, void* data) {
  using namespace aurora::vulkan_win32;
  if (bridge || !cb || !api.Load()) return false;
  bridge = std::make_unique<Bridge>(cb, data);
  aurora::stereo::set_sink(
      [](wgpu::CommandEncoder& encoder, const aurora::stereo::SinkFrame& frame, void* self) noexcept {
        return static_cast<Bridge*>(self)->Encode(encoder, frame);
      }, [](const aurora::stereo::SinkFrame& frame, void* self) noexcept { static_cast<Bridge*>(self)->Submitted(frame); }, bridge.get());
  aurora::stereo::set_direct_targets(
      [](uint64_t token, wgpu::TextureFormat format, uint32_t width, uint32_t height,
         std::array<wgpu::Texture, AURORA_STEREO_EYE_COUNT>& out, void* self) noexcept {
        return static_cast<Bridge*>(self)->DirectTargets(token, format, width, height, out);
      }, bridge.get());
  return true;
}
bool aurora_vulkan_win32_set_targets(uint64_t token, const AuroraD3D12StereoTarget* targets, uint32_t count) {
  using namespace aurora::vulkan_win32;
  return bridge && bridge->Set(token, targets, count, nullptr);
}
bool aurora_vulkan_win32_set_targets_with_panel(uint64_t token, const AuroraD3D12StereoTarget* targets, uint32_t count,
                                                const AuroraD3D12StereoTarget* panel) {
  using namespace aurora::vulkan_win32;
  return bridge && bridge->Set(token, targets, count, panel);
}
bool aurora_vulkan_win32_cancel(uint64_t token) {
  using namespace aurora::vulkan_win32;
  return bridge && bridge->Cancel(token);
}
bool aurora_vulkan_win32_disable() {
  using namespace aurora::vulkan_win32;
  if (!bridge) return true;
  aurora::stereo::set_sink(nullptr, nullptr, nullptr);
  aurora::stereo::set_direct_targets(nullptr, nullptr);
  if (!api.drain(aurora::webgpu::g_device.Get())) { bridge.release(); return false; }
  bridge.reset();
  return true;
}
bool aurora_vulkan_win32_submitting_retained() { return aurora::vulkan_win32::submittingRetained; }
void* aurora_vulkan_win32_lock_queue() {
  return aurora::vulkan_win32::api.lock(aurora::webgpu::g_device.Get());
}
void aurora_vulkan_win32_unlock_queue(void* guard) { aurora::vulkan_win32::api.unlock(guard); }
#else
// C ABI stubs keep the runtime's OpenXR integration linkable on Windows GX
// builds whose Dawn has no Vulkan backend; the backend then reports that the
// bridge is unavailable and the game falls back to the desktop renderer.
bool aurora_vulkan_win32_configure(const AuroraDawnVulkanHooks*) { return false; }
bool aurora_vulkan_win32_get_handles(AuroraDawnVulkanHandles* handles, int64_t* format) {
  if (handles) *handles = {};
  if (format) *format = 0;
  return false;
}
bool aurora_vulkan_win32_enable(AuroraD3D12StereoSubmittedCallback, void*) { return false; }
bool aurora_vulkan_win32_set_targets(uint64_t, const AuroraD3D12StereoTarget*, uint32_t) { return false; }
bool aurora_vulkan_win32_set_targets_with_panel(uint64_t, const AuroraD3D12StereoTarget*, uint32_t,
                                                const AuroraD3D12StereoTarget*) { return false; }
bool aurora_vulkan_win32_cancel(uint64_t) { return false; }
bool aurora_vulkan_win32_disable() { return true; }
bool aurora_vulkan_win32_submitting_retained() { return false; }
void* aurora_vulkan_win32_lock_queue() { return nullptr; }
void aurora_vulkan_win32_unlock_queue(void*) {}
#endif
