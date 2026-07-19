#pragma once

// CRC32 (IEEE 802.3, reflected, polynomial 0xEDB88320) implemented from
// scratch. Used to checksum every SSTable block, the SSTable index, and every
// WAL record so on-disk corruption is caught on read rather than silently
// returning bad data.

#include <cstddef>
#include <cstdint>
#include <string>

namespace lsmdb {

// One-shot CRC32 over a buffer.
uint32_t Crc32(const char* data, size_t n);

inline uint32_t Crc32(const std::string& s) {
  return Crc32(s.data(), s.size());
}

}  // namespace lsmdb
