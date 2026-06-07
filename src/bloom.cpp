#include "lsmdb/bloom.h"

#include <cmath>

#include "lsmdb/coding.h"
#include "lsmdb/hash.h"

namespace lsmdb {

int BloomBuilder::ProbesForBitsPerKey(int bits_per_key) {
  // k = (m/n) * ln2, rounded, clamped to a sane range.
  int k = static_cast<int>(std::lround(bits_per_key * 0.6931471805599453));
  if (k < 1) k = 1;
  if (k > 30) k = 30;
  return k;
}

void BloomBuilder::Add(const std::string& key) {
  uint64_t h[2];
  MurmurHash128(key, h);
  h1_.push_back(h[0]);
  h2_.push_back(h[1]);
}

std::string BloomBuilder::Finish() {
  const size_t n = h1_.size();
  const int k = ProbesForBitsPerKey(bits_per_key_);

  // Round the bit count up to a whole number of bytes; keep a small floor so a
  // tiny table still has a usable filter.
  uint64_t bits = static_cast<uint64_t>(n) * bits_per_key_;
  if (bits < 64) bits = 64;
  size_t bytes = static_cast<size_t>((bits + 7) / 8);
  bits = static_cast<uint64_t>(bytes) * 8;

  std::string out;
  PutFixed32(&out, static_cast<uint32_t>(bits));
  out.push_back(static_cast<char>(k));
  const size_t bit_array_offset = out.size();
  out.resize(bit_array_offset + bytes, '\0');

  auto* array = reinterpret_cast<uint8_t*>(&out[bit_array_offset]);
  for (size_t i = 0; i < n; ++i) {
    uint64_t a = h1_[i];
    const uint64_t b = h2_[i];
    for (int j = 0; j < k; ++j) {
      uint64_t bit = a % bits;            // double hashing: h1 + j*h2
      array[bit >> 3] |= (1u << (bit & 7));
      a += b;
    }
  }

  h1_.clear();
  h2_.clear();
  return out;
}

void BloomFilter::Reset(std::string data) {
  data_ = std::move(data);
  num_bits_ = 0;
  num_probes_ = 0;
  bits_ = nullptr;
  if (data_.size() < 5) return;  // header is u32 + u8
  uint32_t bits = DecodeFixed32(data_.data());
  int k = static_cast<unsigned char>(data_[4]);
  if (bits == 0 || data_.size() < 5 + (bits + 7) / 8) return;
  num_bits_ = bits;
  num_probes_ = k;
  bits_ = reinterpret_cast<const uint8_t*>(data_.data()) + 5;
}

bool BloomFilter::MayContain(const std::string& key) const {
  if (num_bits_ == 0) return true;  // no filter -> can't rule anything out
  uint64_t h[2];
  MurmurHash128(key, h);
  uint64_t a = h[0];
  const uint64_t b = h[1];
  for (int j = 0; j < num_probes_; ++j) {
    uint64_t bit = a % num_bits_;
    if ((bits_[bit >> 3] & (1u << (bit & 7))) == 0) return false;
    a += b;
  }
  return true;
}

}  // namespace lsmdb
