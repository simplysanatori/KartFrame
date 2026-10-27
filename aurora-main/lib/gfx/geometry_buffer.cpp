#include "geometry_buffer.hpp"

#include "../gx/geometry_cache.hpp"
#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "common.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <vector>

namespace aurora::gfx {
namespace {
static Module Log("aurora::gfx");

struct Upload {
  uint32_t src = 0; // in the frame's vertex staging
  uint32_t dst = 0; // in the buffer
  uint32_t size = 0;
};

std::atomic<uint32_t> g_capacityMb{0};
std::atomic<uint64_t> g_capacity{0};
wgpu::Buffer g_buffer;
wgpu::BindGroup g_bindGroup;
std::vector<Upload> g_uploads;
bool g_creationFailed = false;
} // namespace

void set_geometry_buffer_capacity_mb(uint32_t mb) noexcept { g_capacityMb.store(mb, std::memory_order_relaxed); }

uint64_t ensure_geometry_buffer() noexcept {
  static const bool backendSet = [] {
    gx::geometry_cache::set_backend({.capacity = &geometry_buffer_capacity, .upload = &queue_geometry_upload});
    return true;
  }();
  (void)backendSet;
  if (g_buffer) {
    return g_capacity.load(std::memory_order_acquire);
  }
  const uint32_t mb = g_capacityMb.load(std::memory_order_relaxed);
  if (mb == 0 || g_creationFailed || !webgpu::g_device || !g_storageBuffer || !g_staticBindGroupLayout) {
    return 0;
  }
  // Bound whole as storage, so the device's binding limit caps it.
  wgpu::Limits limits{};
  webgpu::g_device.GetLimits(&limits);
  const uint64_t capacity =
      std::min({static_cast<uint64_t>(mb) << 20, static_cast<uint64_t>(limits.maxStorageBufferBindingSize),
                static_cast<uint64_t>(limits.maxBufferSize)}) &
      ~static_cast<uint64_t>(3);
  if (capacity < 65536) {
    g_creationFailed = true;
    return 0;
  }
  const wgpu::BufferDescriptor descriptor{
      .label = "Geometry cache buffer",
      .usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
      .size = capacity,
  };
  g_buffer = webgpu::g_device.CreateBuffer(&descriptor);
  if (!g_buffer) {
    Log.warn("Geometry cache: the device refused a {} MiB buffer; drawing without the cache", capacity >> 20);
    g_creationFailed = true;
    return 0;
  }
  const std::array entries{
      wgpu::BindGroupEntry{.binding = 0, .buffer = g_buffer},
      wgpu::BindGroupEntry{.binding = 1, .buffer = g_storageBuffer},
  };
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .label = "Geometry cache bind group",
      .layout = g_staticBindGroupLayout,
      .entryCount = entries.size(),
      .entries = entries.data(),
  };
  g_bindGroup = webgpu::g_device.CreateBindGroup(&bindGroupDescriptor);
  g_capacity.store(capacity, std::memory_order_release);
  Log.info("Resident geometry: {} MiB buffer created for the geometry cache", capacity >> 20);
  return capacity;
}

uint64_t geometry_buffer_capacity() noexcept { return g_capacity.load(std::memory_order_acquire); }

bool queue_geometry_upload(uint32_t offset, const uint8_t* data, uint32_t size) {
  if (size == 0 || size % 4 != 0 || vertex_staging_room() < size) {
    return false;
  }
  auto [buffer, range] = map_verts(size);
  std::memcpy(buffer.data(), data, size);
  g_uploads.push_back(Upload{.src = range.offset, .dst = offset, .size = size});
  return true;
}

namespace detail {
const wgpu::Buffer& geometry_buffer() noexcept { return g_buffer; }
const wgpu::BindGroup& geometry_bind_group() noexcept { return g_bindGroup; }

void encode_geometry_uploads(const wgpu::CommandEncoder& cmd, const wgpu::Buffer& staging) {
  if (g_buffer) {
    for (const auto& upload : g_uploads) {
      cmd.CopyBufferToBuffer(staging, upload.src, g_buffer, upload.dst, upload.size);
    }
  }
  g_uploads.clear();
}

void drop_geometry_uploads() noexcept { g_uploads.clear(); }

void log_resident_geometry(uint32_t frameIndex) noexcept {
  static auto last = std::chrono::steady_clock::now();
  static uint32_t lastFrame = frameIndex;
  if (!get_gx_deindex_vertices() && g_capacityMb.load(std::memory_order_relaxed) == 0) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(10)) {
    return;
  }
  const double seconds = std::chrono::duration<double>(now - last).count();
  const uint32_t frames = std::max<uint32_t>(frameIndex - lastFrame, 1u);
  last = now;
  lastFrame = frameIndex;
  namespace cache = gx::geometry_cache;
  const auto take = [frames](std::atomic<uint32_t>& counter) {
    return static_cast<double>(counter.exchange(0, std::memory_order_relaxed)) / frames;
  };
  const double hits = take(cache::g_hits);
  const double native = take(cache::g_nativeDraws);
  const double revalidated = take(cache::g_revalidated);
  const double misses = take(cache::g_misses);
  const double reresolved = take(cache::g_reresolved);
  const double unsupported = take(cache::g_inPlaceUnsupported);
  const double untracked = take(cache::g_inPlaceUntracked);
  const double full = take(cache::g_inPlaceFull);
  const double volatiles = take(cache::g_inPlaceVolatile);
  const auto stats = cache::stats();
  const uint32_t peakVertices = g_stagingStats.peakVertexBytes.exchange(0, std::memory_order_relaxed);
  const uint32_t peakIndices = g_stagingStats.peakIndexBytes.exchange(0, std::memory_order_relaxed);
  const uint32_t stops = g_stagingStats.deindexStops.exchange(0, std::memory_order_relaxed);
  const uint32_t skipped = g_stagingStats.skippedDraws.exchange(0, std::memory_order_relaxed);
  Log.info("Resident geometry ({:.0f} s, {} frames), a frame: {:.1f} lists from the cache ({:.1f} native, {:.2f} after "
           "a re-digest), {:.2f} resolved into it ({:.2f} replacing changed bytes); in place {:.1f} unsupported, {:.1f} "
           "untracked, {:.2f} no room, {:.2f} rewritten by the game. {} entries, {:.1f} of {} MiB resident, {} "
           "retired blocks, {} free blocks, {} volatile keys. Staging high water: vertices {} of {} KiB, indices {} of "
           "{} KiB; de-indexing stopped early on {} frames, {} draws skipped",
           seconds, frames, hits, native, revalidated, misses, reresolved, unsupported, untracked, full, volatiles,
           stats.entries, static_cast<double>(stats.residentBytes) / (1024.0 * 1024.0), stats.capacity >> 20,
           stats.retiredBlocks, stats.freeBlocks, stats.volatileKeys, peakVertices >> 10, VertexBufferSize >> 10,
           peakIndices >> 10, IndexBufferSize >> 10, stops, skipped);
}

void shutdown_geometry_buffer() noexcept {
  g_uploads.clear();
  g_capacity.store(0, std::memory_order_release);
  g_bindGroup = {};
  g_buffer = {};
  g_creationFailed = false;
}
} // namespace detail

} // namespace aurora::gfx

void aurora_set_gx_deindex_vertices(bool enabled) { aurora::gfx::set_gx_deindex_vertices(enabled); }
void aurora_set_gx_geometry_cache_mb(uint32_t mb) { aurora::gfx::set_geometry_buffer_capacity_mb(mb); }
void aurora_set_gx_native_vertex_input(bool enabled) { aurora::gfx::set_gx_native_vertex_input(enabled); }
void aurora_request_geometry_cache_clear(void) { aurora::gx::geometry_cache::request_clear(); }
