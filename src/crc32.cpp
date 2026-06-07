#include "lsmdb/crc32.h"

#include <array>

namespace lsmdb {
namespace {

// Precompute the 256-entry lookup table for the reflected polynomial once at
// startup. Byte-at-a-time is plenty fast for our block sizes and keeps the
// implementation self-contained (no zlib/crc32c dependency).
std::array<uint32_t, 256> BuildTable() {
  std::array<uint32_t, 256> table{};
  const uint32_t kPoly = 0xEDB88320u;
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1) ? (kPoly ^ (c >> 1)) : (c >> 1);
    }
    table[i] = c;
  }
  return table;
}

const std::array<uint32_t, 256>& Table() {
  static const std::array<uint32_t, 256> kTable = BuildTable();
  return kTable;
}

}  // namespace

uint32_t Crc32(const char* data, size_t n) {
  const auto& table = Table();
  uint32_t crc = 0xFFFFFFFFu;
  const auto* p = reinterpret_cast<const unsigned char*>(data);
  for (size_t i = 0; i < n; ++i) {
    crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

}  // namespace lsmdb
