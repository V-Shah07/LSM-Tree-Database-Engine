#pragma once

// Bloom filter with a tunable false-positive rate, attached to each SSTable.
//
// A bloom filter answers "is this key definitely absent?" in O(k) with no disk
// access. On a point lookup we consult it before reading any data block: if it
// says "no", the SSTable is skipped entirely; if it says "maybe", we fall back
// to the sparse index + block scan. Since most point lookups in an LSM tree are
// for keys not in a given SSTable, this eliminates the large majority of
// otherwise-wasted block reads.
//
// Sizing: with m bits over n keys the optimal probe count is k = (m/n)·ln2, and
// the expected false-positive rate is (1 − e^(−kn/m))^k. `bits_per_key` (m/n)
// is the single tuning knob — 10 bits/key gives ≈1% FP.
//
// Serialized layout:
//   u32 num_bits
//   u8  num_probes (k)
//   ceil(num_bits/8) bytes of bit array

#include <cstdint>
#include <string>
#include <vector>

namespace lsmdb {

// Accumulates key hashes and emits a serialized filter. Stores only the 128-bit
// hash per key (16 bytes), never the key itself, so building a filter over a
// large SSTable is cheap in memory.
class BloomBuilder {
 public:
  explicit BloomBuilder(int bits_per_key = 10) : bits_per_key_(bits_per_key) {}

  void Add(const std::string& key);
  size_t NumKeys() const { return h1_.size(); }

  // Emit the serialized filter and reset the builder.
  std::string Finish();

  // Optimal probe count for a given bits/key, clamped to [1, 30].
  static int ProbesForBitsPerKey(int bits_per_key);

 private:
  int bits_per_key_;
  std::vector<uint64_t> h1_;
  std::vector<uint64_t> h2_;
};

// Immutable, queryable filter parsed from the bytes BloomBuilder produced.
class BloomFilter {
 public:
  BloomFilter() = default;
  explicit BloomFilter(std::string data) { Reset(std::move(data)); }

  void Reset(std::string data);

  // False -> key is definitely not present. True -> possibly present.
  bool MayContain(const std::string& key) const;

  bool valid() const { return num_bits_ > 0; }
  uint32_t num_bits() const { return num_bits_; }
  int num_probes() const { return num_probes_; }

 private:
  std::string data_;   // full serialized blob; bit array starts at offset 5
  const uint8_t* bits_ = nullptr;
  uint32_t num_bits_ = 0;
  int num_probes_ = 0;
};

}  // namespace lsmdb
