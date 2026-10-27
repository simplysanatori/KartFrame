// Resident geometry (lane AC): aurora's geometry cache (aurora-main/lib/gx/geometry_cache.cpp), driven
// without a GPU through its backend hooks. Covers the record validation (guest-write generations and the
// list digest), volatile lists, block placement and reuse only after the retire delay, dropped uploads,
// the race-exit clear and a full buffer.
#include "gx/geometry_cache.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace cache = aurora::gx::geometry_cache;

static int g_failures = 0;
#define EXPECT(cond)                                                                                                   \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, #cond);                                   \
      ++g_failures;                                                                                                    \
    }                                                                                                                  \
  } while (0)

static uint64_t g_capacity = 4096;
static bool g_stagingRoom = true;
struct Upload {
  uint32_t offset;
  std::vector<uint8_t> bytes;
};
static std::vector<Upload> g_uploads;

static uint64_t capacity() noexcept { return g_capacity; }
static bool upload(uint32_t offset, const uint8_t* data, uint32_t size) {
  if (!g_stagingRoom) return false;
  g_uploads.push_back({offset, std::vector<uint8_t>(data, data + size)});
  return true;
}

static cache::Entry* insert(const cache::Key& key, uint32_t stride, uint32_t vertices, uint64_t listGen,
                            uint64_t digest, uint64_t arrayGen, cache::InsertResult* result = nullptr) {
  cache::Entry entry{};
  entry.stride = stride;
  entry.listGeneration = listGen;
  entry.listDigest = digest;
  entry.arrayGenerations[1] = arrayGen;
  std::vector<uint8_t> records(static_cast<size_t>(stride) * vertices, 0xAB);
  std::vector<uint32_t> indices;
  for (uint32_t v = 0; v + 2 < vertices; v += 3) indices.insert(indices.end(), {v, v + 1, v + 2});
  return cache::insert(key, std::move(entry), records, indices, result);
}

int main() {
  cache::set_backend({.capacity = &capacity, .upload = &upload});
  uint32_t frame = 100;
  cache::begin_frame(frame);

  const cache::Key a{.list = 0x1000, .layout = 7, .nbytes = 64};
  const cache::Key b{.list = 0x2000, .layout = 7, .nbytes = 64};

  // Insert: placed at a multiple of the stride, indices made absolute, uploaded once.
  cache::Entry* ea = insert(a, 12, 6, /*listGen*/ 5, /*digest*/ 42, /*arrayGen*/ 9);
  EXPECT(ea != nullptr && ea->offset == 0 && ea->bytes == 72 && ea->vertexCount == 6);
  cache::Entry* eb = insert(b, 20, 3, 5, 43, 9);
  EXPECT(eb != nullptr && eb->offset % 20 == 0 && eb->offset >= 72);
  EXPECT(eb != nullptr && eb->indices[0] == eb->offset / 20);
  EXPECT(g_uploads.size() == 2);
  cache::uploads_encoded();
  EXPECT(cache::find(a) == ea);

  // Validation. Same generations: valid, no digest computed.
  int digests = 0;
  std::vector<uint64_t> arrays(cache::kMaxAttributes, 0);
  arrays[1] = 9;
  EXPECT(cache::validate(*ea, 5, arrays, [&] { ++digests; return uint64_t{42}; }) == cache::Validity::Valid);
  EXPECT(digests == 0);
  // The list's granule moved (a neighbour written) but its bytes did not: revalidated, new generation kept.
  EXPECT(cache::validate(*ea, 6, arrays, [&] { ++digests; return uint64_t{42}; }) == cache::Validity::Revalidated);
  EXPECT(digests == 1 && ea->listGeneration == 6);
  EXPECT(cache::validate(*ea, 6, arrays, [&] { ++digests; return uint64_t{42}; }) == cache::Validity::Valid);
  // The list was rewritten: changed.
  EXPECT(cache::validate(*ea, 7, arrays, [&] { return uint64_t{99}; }) == cache::Validity::Changed);
  // An indexed array was written (the list bytes alone cannot show it): changed, whatever the digest says.
  arrays[1] = 10;
  digests = 0;
  EXPECT(cache::validate(*ea, 6, arrays, [&] { ++digests; return uint64_t{42}; }) == cache::Validity::Changed);
  EXPECT(digests == 0);

  // A changed entry goes; its block is not reused until kRetireFrames frames later.
  const uint32_t oldOffset = ea->offset;
  cache::erase(a);
  cache::note_changed(a);
  EXPECT(cache::find(a) == nullptr);
  EXPECT(!cache::is_volatile(a));
  const cache::Key c{.list = 0x3000, .layout = 7, .nbytes = 64};
  cache::Entry* ec = insert(c, 12, 6, 1, 1, 1);
  EXPECT(ec != nullptr && ec->offset != oldOffset);
  cache::uploads_encoded();
  for (uint32_t i = 0; i < cache::kRetireFrames; ++i) cache::begin_frame(++frame);
  const cache::Key d{.list = 0x4000, .layout = 7, .nbytes = 64};
  cache::Entry* ed = insert(d, 12, 6, 1, 1, 1);
  EXPECT(ed != nullptr && ed->offset == oldOffset); // first fit, released by now
  cache::uploads_encoded();

  // A second change within kVolatileFrames: volatile, left to the plain path; forgiven after a quiet spell.
  cache::note_changed(a);
  EXPECT(cache::is_volatile(a));
  frame += cache::kVolatileFrames + 1;
  cache::begin_frame(frame);
  EXPECT(!cache::is_volatile(a));

  // Uploads dropped (an aborted frame): the entries inserted since the last seal go, their blocks free at once.
  const auto before = cache::stats();
  cache::Entry* ee = insert(a, 12, 6, 1, 1, 1);
  EXPECT(ee != nullptr);
  cache::uploads_dropped();
  EXPECT(cache::find(a) == nullptr);
  EXPECT(cache::stats().residentBytes == before.residentBytes);

  // No staging room this frame: nothing stored, the block freed again.
  g_stagingRoom = false;
  cache::InsertResult why{};
  EXPECT(insert(a, 12, 6, 1, 1, 1, &why) == nullptr && why == cache::InsertResult::NoStaging);
  g_stagingRoom = true;
  EXPECT(cache::stats().residentBytes == before.residentBytes);

  // Records that are not whole words, or a stride that does not divide them, are refused.
  cache::Entry odd{};
  odd.stride = 6;
  std::vector<uint8_t> oddRecords(18, 0);
  std::vector<uint32_t> oddIndices{0, 1, 2};
  EXPECT(cache::insert(a, std::move(odd), oddRecords, oddIndices, &why) == nullptr &&
         why == cache::InsertResult::Invalid);

  // Full: a list larger than what is left is refused with NoRoom.
  EXPECT(insert(a, 16, 300, 1, 1, 1, &why) == nullptr && why == cache::InsertResult::NoRoom);

  // The race-exit clear: applied at the next frame begin, every block retired, then released.
  cache::request_clear();
  EXPECT(cache::find(b) != nullptr);
  cache::begin_frame(++frame);
  EXPECT(cache::find(b) == nullptr && cache::stats().entries == 0 && cache::stats().residentBytes == 0);
  for (uint32_t i = 0; i < cache::kRetireFrames; ++i) cache::begin_frame(++frame);
  EXPECT(cache::stats().freeBlocks == 1 && cache::stats().retiredBlocks == 0);

  // The buffer going away (capacity change) drops everything.
  insert(a, 12, 6, 1, 1, 1);
  g_capacity = 0;
  EXPECT(cache::find(a) == nullptr && cache::stats().entries == 0);

  if (g_failures != 0) {
    std::fprintf(stderr, "geometry cache tests: %d failures\n", g_failures);
    return 1;
  }
  std::printf("geometry cache tests passed\n");
  return 0;
}
