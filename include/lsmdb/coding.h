#pragma once

// Little-endian fixed-width encode/decode helpers shared by the SSTable format
// and the write-ahead log. Little-endian is chosen to match the target x86-64
// hardware so decoding is effectively a load.

#include <cstdint>
#include <cstring>
#include <string>

namespace lsmdb {

inline void PutFixed32(std::string* dst, uint32_t v) {
  char buf[4];
  buf[0] = static_cast<char>(v & 0xff);
  buf[1] = static_cast<char>((v >> 8) & 0xff);
  buf[2] = static_cast<char>((v >> 16) & 0xff);
  buf[3] = static_cast<char>((v >> 24) & 0xff);
  dst->append(buf, sizeof(buf));
}

inline void PutFixed64(std::string* dst, uint64_t v) {
  char buf[8];
  for (int i = 0; i < 8; ++i) {
    buf[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  }
  dst->append(buf, sizeof(buf));
}

inline uint32_t DecodeFixed32(const char* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));  // x86-64 is little-endian; direct load
  return v;
}

inline uint64_t DecodeFixed64(const char* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

// Length-prefixed string: u32 length followed by the bytes.
inline void PutLengthPrefixed(std::string* dst, const std::string& s) {
  PutFixed32(dst, static_cast<uint32_t>(s.size()));
  dst->append(s);
}

}  // namespace lsmdb
