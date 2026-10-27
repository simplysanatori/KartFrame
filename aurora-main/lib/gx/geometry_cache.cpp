#include "geometry_cache.hpp"

#include <algorithm>
#include <unordered_map>

// Ported from the iChris4/Wiicompiled_VR fork (b5d9c19, GPL-3.0); see geometry_cache.hpp.
namespace aurora::gx::geometry_cache {

std::atomic<uint32_t> g_hits{0};
std::atomic<uint32_t> g_misses{0};
std::atomic<uint32_t> g_revalidated{0};
std::atomic<uint32_t> g_reresolved{0};
std::atomic<uint32_t> g_inPlaceUnsupported{0};
std::atomic<uint32_t> g_inPlaceUntracked{0};
std::atomic<uint32_t> g_inPlaceFull{0};
std::atomic<uint32_t> g_inPlaceVolatile{0};
std::atomic<uint32_t> g_nativeDraws{0};

namespace {
struct FreeBlock {
  uint32_t offset;
  uint32_t size;
};
struct RetiredBlock {
  uint32_t offset;
  uint32_t size;
  uint32_t releaseFrame;
};
// Lists whose bytes changed under the cache: how often, and when last (is_volatile).
struct Volatility {
  uint32_t strikes = 0;
  uint32_t lastFrame = 0;
};

Backend g_backend{};
std::unordered_map<Key, Entry, KeyHash> g_entries;
std::unordered_map<Key, Volatility, KeyHash> g_volatile;
std::vector<FreeBlock> g_free; // by offset, neighbours merged
std::vector<RetiredBlock> g_retired;
std::vector<Key> g_pendingKeys; // inserted since the last seal, their uploads not yet encoded
std::atomic<bool> g_clearRequested{false};
uint32_t g_frame = 0;
uint64_t g_capacity = 0;
uint64_t g_residentBytes = 0;

void reset_blocks(uint64_t capacity) noexcept {
  g_entries.clear();
  g_volatile.clear();
  g_free.clear();
  g_retired.clear();
  g_pendingKeys.clear();
  g_capacity = capacity;
  g_residentBytes = 0;
  if (capacity != 0) {
    g_free.push_back({0, static_cast<uint32_t>(std::min<uint64_t>(capacity, UINT32_MAX))});
  }
}

// The buffer may appear or go between frames (its lazy creation, a device reset): the entries describe the old one.
void sync_capacity() noexcept {
  const uint64_t capacity = g_backend.capacity != nullptr ? g_backend.capacity() : 0;
  if (capacity != g_capacity) {
    reset_blocks(capacity);
  }
}

// First fit, at a multiple of `stride` so an absolute index is whole.
bool allocate(uint32_t bytes, uint32_t stride, uint32_t& out) noexcept {
  for (size_t i = 0; i < g_free.size(); ++i) {
    const FreeBlock block = g_free[i];
    const uint64_t start = (static_cast<uint64_t>(block.offset) + stride - 1) / stride * stride;
    const uint64_t end = start + bytes;
    const uint64_t blockEnd = static_cast<uint64_t>(block.offset) + block.size;
    if (end > blockEnd) {
      continue;
    }
    out = static_cast<uint32_t>(start);
    const uint32_t front = static_cast<uint32_t>(start - block.offset);
    const uint32_t tail = static_cast<uint32_t>(blockEnd - end);
    if (front != 0 && tail != 0) {
      g_free[i] = {block.offset, front};
      g_free.insert(g_free.begin() + static_cast<std::ptrdiff_t>(i) + 1, FreeBlock{static_cast<uint32_t>(end), tail});
    } else if (front != 0) {
      g_free[i] = {block.offset, front};
    } else if (tail != 0) {
      g_free[i] = {static_cast<uint32_t>(end), tail};
    } else {
      g_free.erase(g_free.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return true;
  }
  return false;
}

void release(uint32_t offset, uint32_t size) noexcept {
  auto it = std::lower_bound(g_free.begin(), g_free.end(), offset,
                             [](const FreeBlock& block, uint32_t value) { return block.offset < value; });
  it = g_free.insert(it, FreeBlock{offset, size});
  if (it + 1 != g_free.end() && it->offset + it->size == (it + 1)->offset) {
    it->size += (it + 1)->size;
    g_free.erase(it + 1);
  }
  if (it != g_free.begin() && (it - 1)->offset + (it - 1)->size == it->offset) {
    (it - 1)->size += it->size;
    g_free.erase(it);
  }
}

void retire(const Entry& entry) noexcept {
  g_retired.push_back({entry.offset, entry.bytes, g_frame + kRetireFrames});
  g_residentBytes -= entry.bytes;
}
} // namespace

void set_backend(const Backend& backend) noexcept { g_backend = backend; }

Entry* find(const Key& key) noexcept {
  sync_capacity();
  const auto it = g_entries.find(key);
  return it != g_entries.end() ? &it->second : nullptr;
}

Entry* insert(const Key& key, Entry&& entry, std::span<const uint8_t> records, const std::vector<uint32_t>& indices,
              InsertResult* result) {
  const auto fail = [result](InsertResult why) -> Entry* {
    if (result != nullptr) {
      *result = why;
    }
    return nullptr;
  };
  sync_capacity();
  const uint32_t bytes = static_cast<uint32_t>(records.size());
  if (g_capacity == 0 || entry.stride == 0 || bytes == 0 || bytes % 4 != 0 || bytes % entry.stride != 0 ||
      indices.empty() || g_backend.upload == nullptr) {
    return fail(InsertResult::Invalid);
  }
  if (const auto old = g_entries.find(key); old != g_entries.end()) {
    retire(old->second);
    g_entries.erase(old);
  }
  uint32_t offset = 0;
  if (!allocate(bytes, entry.stride, offset)) {
    return fail(InsertResult::NoRoom);
  }
  if (!g_backend.upload(offset, records.data(), bytes)) {
    release(offset, bytes);
    return fail(InsertResult::NoStaging);
  }
  entry.offset = offset;
  entry.bytes = bytes;
  entry.vertexCount = bytes / entry.stride;
  const uint32_t base = offset / entry.stride;
  entry.indices.resize(indices.size());
  for (size_t i = 0; i < indices.size(); ++i) {
    entry.indices[i] = base + indices[i];
  }
  g_residentBytes += bytes;
  g_pendingKeys.push_back(key);
  const auto [it, inserted] = g_entries.insert_or_assign(key, std::move(entry));
  if (result != nullptr) {
    *result = InsertResult::Inserted;
  }
  return &it->second;
}

void erase(const Key& key) noexcept {
  const auto it = g_entries.find(key);
  if (it == g_entries.end()) {
    return;
  }
  retire(it->second);
  g_entries.erase(it);
}

void note_changed(const Key& key) noexcept {
  auto& v = g_volatile[key];
  if (v.strikes != 0 && g_frame - v.lastFrame > kVolatileFrames) {
    v.strikes = 0;
  }
  ++v.strikes;
  v.lastFrame = g_frame;
}

bool is_volatile(const Key& key) noexcept {
  const auto it = g_volatile.find(key);
  if (it == g_volatile.end()) {
    return false;
  }
  if (g_frame - it->second.lastFrame > kVolatileFrames) {
    g_volatile.erase(it);
    return false;
  }
  return it->second.strikes >= 2;
}

void request_clear() noexcept { g_clearRequested.store(true, std::memory_order_release); }

void begin_frame(uint32_t frameIndex) noexcept {
  g_frame = frameIndex;
  if (g_clearRequested.exchange(false, std::memory_order_acq_rel)) {
    for (auto& [key, entry] : g_entries) {
      retire(entry);
    }
    g_entries.clear();
    g_pendingKeys.clear();
    // The addresses may hold another course's lists next: judge them afresh.
    g_volatile.clear();
  }
  // Release what no frame in flight can still read.
  for (size_t i = 0; i < g_retired.size();) {
    if (static_cast<int32_t>(g_frame - g_retired[i].releaseFrame) >= 0) {
      release(g_retired[i].offset, g_retired[i].size);
      g_retired[i] = g_retired.back();
      g_retired.pop_back();
    } else {
      ++i;
    }
  }
}

void uploads_encoded() noexcept { g_pendingKeys.clear(); }

void uploads_dropped() noexcept {
  // Their blocks hold nothing a frame in flight reads, so they can be reused at once.
  for (const Key& key : g_pendingKeys) {
    const auto it = g_entries.find(key);
    if (it == g_entries.end()) {
      continue;
    }
    g_residentBytes -= it->second.bytes;
    release(it->second.offset, it->second.bytes);
    g_entries.erase(it);
  }
  g_pendingKeys.clear();
}

void clear() noexcept { reset_blocks(0); }

Stats stats() noexcept {
  return {
      .entries = static_cast<uint32_t>(g_entries.size()),
      .residentBytes = g_residentBytes,
      .capacity = g_capacity,
      .retiredBlocks = static_cast<uint32_t>(g_retired.size()),
      .freeBlocks = static_cast<uint32_t>(g_free.size()),
      .volatileKeys = static_cast<uint32_t>(g_volatile.size()),
  };
}

} // namespace aurora::gx::geometry_cache
