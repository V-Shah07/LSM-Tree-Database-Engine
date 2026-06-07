#include "lsmdb/hash.h"

#include <cstring>

namespace lsmdb {
namespace {

inline uint64_t Rotl64(uint64_t x, int8_t r) {
  return (x << r) | (x >> (64 - r));
}

// Read a 64-bit block via memcpy: SSTable data comes from an mmap and may be
// unaligned, so we must not cast-and-deref.
inline uint64_t GetBlock64(const uint8_t* p, int i) {
  uint64_t v;
  std::memcpy(&v, p + i * 8, sizeof(v));
  return v;
}

inline uint64_t Fmix64(uint64_t k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdULL;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53ULL;
  k ^= k >> 33;
  return k;
}

}  // namespace

void MurmurHash3_x64_128(const void* key, size_t len, uint32_t seed,
                         uint64_t out[2]) {
  const auto* data = static_cast<const uint8_t*>(key);
  const size_t nblocks = len / 16;

  uint64_t h1 = seed;
  uint64_t h2 = seed;
  const uint64_t c1 = 0x87c37b91114253d5ULL;
  const uint64_t c2 = 0x4cf5ad432745937fULL;

  // Body: consume 16 bytes at a time.
  for (size_t i = 0; i < nblocks; ++i) {
    uint64_t k1 = GetBlock64(data, static_cast<int>(i * 2 + 0));
    uint64_t k2 = GetBlock64(data, static_cast<int>(i * 2 + 1));

    k1 *= c1; k1 = Rotl64(k1, 31); k1 *= c2; h1 ^= k1;
    h1 = Rotl64(h1, 27); h1 += h2; h1 = h1 * 5 + 0x52dce729;

    k2 *= c2; k2 = Rotl64(k2, 33); k2 *= c1; h2 ^= k2;
    h2 = Rotl64(h2, 31); h2 += h1; h2 = h2 * 5 + 0x38495ab5;
  }

  // Tail: the remaining 0..15 bytes.
  const uint8_t* tail = data + nblocks * 16;
  uint64_t k1 = 0;
  uint64_t k2 = 0;
  switch (len & 15) {
    case 15: k2 ^= static_cast<uint64_t>(tail[14]) << 48; [[fallthrough]];
    case 14: k2 ^= static_cast<uint64_t>(tail[13]) << 40; [[fallthrough]];
    case 13: k2 ^= static_cast<uint64_t>(tail[12]) << 32; [[fallthrough]];
    case 12: k2 ^= static_cast<uint64_t>(tail[11]) << 24; [[fallthrough]];
    case 11: k2 ^= static_cast<uint64_t>(tail[10]) << 16; [[fallthrough]];
    case 10: k2 ^= static_cast<uint64_t>(tail[9]) << 8;  [[fallthrough]];
    case 9:  k2 ^= static_cast<uint64_t>(tail[8]) << 0;
             k2 *= c2; k2 = Rotl64(k2, 33); k2 *= c1; h2 ^= k2;
             [[fallthrough]];
    case 8:  k1 ^= static_cast<uint64_t>(tail[7]) << 56; [[fallthrough]];
    case 7:  k1 ^= static_cast<uint64_t>(tail[6]) << 48; [[fallthrough]];
    case 6:  k1 ^= static_cast<uint64_t>(tail[5]) << 40; [[fallthrough]];
    case 5:  k1 ^= static_cast<uint64_t>(tail[4]) << 32; [[fallthrough]];
    case 4:  k1 ^= static_cast<uint64_t>(tail[3]) << 24; [[fallthrough]];
    case 3:  k1 ^= static_cast<uint64_t>(tail[2]) << 16; [[fallthrough]];
    case 2:  k1 ^= static_cast<uint64_t>(tail[1]) << 8;  [[fallthrough]];
    case 1:  k1 ^= static_cast<uint64_t>(tail[0]) << 0;
             k1 *= c1; k1 = Rotl64(k1, 31); k1 *= c2; h1 ^= k1;
  }

  // Finalization.
  h1 ^= len;
  h2 ^= len;
  h1 += h2;
  h2 += h1;
  h1 = Fmix64(h1);
  h2 = Fmix64(h2);
  h1 += h2;
  h2 += h1;

  out[0] = h1;
  out[1] = h2;
}

}  // namespace lsmdb
