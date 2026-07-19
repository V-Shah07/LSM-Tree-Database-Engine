#pragma once

// MurmurHash3 (x64, 128-bit variant), implemented from scratch. Fast,
// well-distributed, non-cryptographic. The bloom filter uses the two 64-bit
// halves of the 128-bit output as (h1, h2) for double hashing, which lets it
// synthesize k independent probes from a single hash computation.

#include <cstddef>
#include <cstdint>
#include <string>

namespace lsmdb {

// Writes a 128-bit hash of [data, data+len) into out[0], out[1].
void MurmurHash3_x64_128(const void* data, size_t len, uint32_t seed,
                         uint64_t out[2]);

inline void MurmurHash128(const std::string& s, uint64_t out[2]) {
  MurmurHash3_x64_128(s.data(), s.size(), /*seed=*/0, out);
}

}  // namespace lsmdb
