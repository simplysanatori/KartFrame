#pragma once
// Static display lists kept on the GPU across frames (command_processor.cpp process_display_list).
//
// A display list that only draws (a BRRES shape's primitive list: the course, the karts, the characters) is resolved
// once into flat de-indexed records (command_processor.cpp deindex_plan's layout) in one persistent buffer
// (gfx/geometry_buffer.hpp), its triangle-list indices kept absolute into that buffer. Each frame a hit only stages
// those indices; as every block sits at a multiple of its stride, the surfaces of one material still merge into one
// draw. Keyed by the list's host address and size and the layout the records were resolved with (the format, the
// arrays and where each attribute lands); valid while the guest-RAM write tracking says neither the list nor the
// arrays it indexes moved, a moved list re-digested before it is dropped (validate below).
//
// Ported from the iChris4/Wiicompiled_VR fork (b5d9c19, GPL-3.0), itself after the Metroid Prime port's design. This
// module is self-contained (no renderer headers) so the runtime's unit tests can drive it: the buffer is reached
// through set_backend. GX thread only, except request_clear and the counters.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aurora::gx::geometry_cache {

// GX_VA_PNMTXIDX .. GX_VA_TEX7: the most attributes a de-index plan holds.
inline constexpr size_t kMaxAttributes = 21;
// Frames whose GPU work may still read a block after its entry went: the staging slots rotate through three frames
// (gfx/common.cpp FrameSlotCount) and the eye replays of a sealed frame run within them; one more for margin.
inline constexpr uint32_t kRetireFrames = 4;
// A second change within this many frames marks a list volatile for as long again after its last change.
inline constexpr uint32_t kVolatileFrames = 600;

struct Key {
  uint64_t list = 0;   // the display list's host address
  uint64_t layout = 0; // the resolved layout (command_processor.cpp deindex_plan_hash), salted for native records
  uint32_t nbytes = 0; // its size
  bool operator==(const Key&) const = default;
};
struct KeyHash {
  size_t operator()(const Key& key) const noexcept {
    uint64_t h = key.list * 0x9E3779B97F4A7C15ull;
    h ^= key.layout + 0x7F4A7C159E3779B9ull + (h << 6) + (h >> 2);
    h ^= static_cast<uint64_t>(key.nbytes) * 0xC2B2AE3D27D4EB4Full;
    return static_cast<size_t>(h ^ (h >> 31));
  }
};

struct Entry {
  uint32_t offset = 0; // in the geometry buffer, a multiple of stride
  uint32_t bytes = 0;  // of records, a multiple of 4
  uint32_t stride = 0;
  uint32_t vertexCount = 0;
  std::vector<uint32_t> indices; // triangle list, absolute into the buffer
  // Validity: the write-tracking generations sampled before the bytes were read, and the list's digest for a
  // generation that moved (a neighbouring write bumps the same 4 KiB granule).
  uint64_t listGeneration = 0;
  uint64_t listDigest = 0;
  std::array<uint64_t, kMaxAttributes> arrayGenerations{};
  // Frame interpolation identity of the list's first draw command, and the position-matrix slots all its draws use,
  // so a hit's draw matches what the uncached path would have recorded.
  uint64_t geometrySignature = 0;
  uint64_t topologySignature = 0;
  uint16_t firstMask = 0;
  uint16_t unionMask = 0;
  uint16_t firstVtxCount = 0;
  uint8_t firstPrim = 0;
};

// Where the records go: the buffer's size in bytes (0: none, the cache stays empty) and a copy of `size` bytes to
// `offset` of it, through the frame's staging (false: no room this frame).
struct Backend {
  uint64_t (*capacity)() noexcept = nullptr;
  bool (*upload)(uint32_t offset, const uint8_t* data, uint32_t size) = nullptr;
};
void set_backend(const Backend& backend) noexcept;

enum class InsertResult : uint8_t { Inserted, NoRoom, NoStaging, Invalid };

// The entry for a key, or null.
Entry* find(const Key& key) noexcept;
// Stores a list's resolved records and their triangle-list indices (relative to the first record here, absolute in
// the entry), uploading the records through the backend. Null (and why in `result`) when the buffer has no room, the
// staging none, or the records are not whole 4-byte words: the list then draws the plain way.
Entry* insert(const Key& key, Entry&& entry, std::span<const uint8_t> records, const std::vector<uint32_t>& indices,
              InsertResult* result = nullptr);
// Drops one entry; its block is reused only once the frames that may still draw it are done (kRetireFrames).
void erase(const Key& key) noexcept;
// A list whose bytes changed under the cache (the game rewrites some lists every frame): after a second change in
// quick succession the key is left to the plain path for a while rather than resolved and uploaded each frame.
void note_changed(const Key& key) noexcept;
bool is_volatile(const Key& key) noexcept;
// Asks for every entry to go (a race ends); applied at the next frame begin. Any thread.
void request_clear() noexcept;
// Frame begin on the GX thread: applies a requested clear, releases blocks retired long enough ago.
void begin_frame(uint32_t frameIndex) noexcept;
// The frame's uploads were encoded (end_batch_impl): the entries inserted this batch are final.
void uploads_encoded() noexcept;
// The frame's uploads were dropped (abort_frame): the entries inserted this batch never reached the GPU.
void uploads_dropped() noexcept;
// Drops everything at once (the device or the buffer goes).
void clear() noexcept;

enum class Validity : uint8_t { Valid, Revalidated, Changed };
// Whether `entry` still describes the bytes: every indexed array's generation as sampled when it was resolved (an
// array the game rewrote cannot be told apart by the list's bytes), and the list's generation, or, when that moved
// (a neighbouring write in the same granule), the list's digest, which `digest()` computes only then. A revalidated
// entry takes the new list generation. Generations are sampled by the caller before it reads any byte, so a racing
// guest write costs a re-resolve, never a stale hit.
template <typename Digest>
Validity validate(Entry& entry, uint64_t listGeneration, std::span<const uint64_t> arrayGenerations,
                  Digest&& digest) {
  for (size_t k = 0; k < arrayGenerations.size() && k < entry.arrayGenerations.size(); ++k) {
    if (entry.arrayGenerations[k] != arrayGenerations[k]) {
      return Validity::Changed;
    }
  }
  if (entry.listGeneration == listGeneration) {
    return Validity::Valid;
  }
  if (digest() != entry.listDigest) {
    return Validity::Changed;
  }
  entry.listGeneration = listGeneration;
  return Validity::Revalidated;
}

struct Stats {
  uint32_t entries = 0;
  uint64_t residentBytes = 0;
  uint64_t capacity = 0;
  uint32_t retiredBlocks = 0;
  uint32_t freeBlocks = 0;
  uint32_t volatileKeys = 0;
};
Stats stats() noexcept;

// Per-window counters for the periodic log line, reset by the reader.
extern std::atomic<uint32_t> g_hits;
extern std::atomic<uint32_t> g_misses;
extern std::atomic<uint32_t> g_revalidated;        // hits whose list generation moved but whose bytes had not
extern std::atomic<uint32_t> g_reresolved;         // entries dropped for changed bytes
extern std::atomic<uint32_t> g_inPlaceUnsupported; // lists the cache cannot take (register writes, lines...)
extern std::atomic<uint32_t> g_inPlaceUntracked;   // lists or arrays outside the write-tracked guest RAM
extern std::atomic<uint32_t> g_inPlaceFull;        // no block or no staging room this frame
extern std::atomic<uint32_t> g_inPlaceVolatile;    // lists the game keeps rewriting (is_volatile)
extern std::atomic<uint32_t> g_nativeDraws;        // lists drawn through native vertex input (gx/native_vertex.hpp)

} // namespace aurora::gx::geometry_cache
