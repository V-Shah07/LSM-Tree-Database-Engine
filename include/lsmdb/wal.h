#pragma once

// Write-Ahead Log. Every mutation is appended here (and fsync'd) *before* it is
// applied to the memtable, so a crash can never lose a write the caller was
// told succeeded. On restart the log is replayed to rebuild the memtable.
//
// Record framing (all integers little-endian):
//   u32 crc32(payload)
//   u32 payload_length
//   payload:
//     u8  type (0=value, 1=tombstone)
//     u32 key_len,   key bytes
//     u32 value_len, value bytes
//
// The length prefix lets the reader skip a record whose payload is torn, and
// the CRC lets it detect a partially-written tail record left behind by a crash
// mid-write. Replay stops at the first such record (there can only be one, at
// the very end) and treats everything before it as durable.

#include <cstdint>
#include <functional>
#include <string>

#include "lsmdb/sstable.h"  // ValueType
#include "lsmdb/status.h"

namespace lsmdb {

class WalWriter {
 public:
  WalWriter() = default;
  ~WalWriter();

  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  // Open for appending (creates the file if absent; never truncates).
  Status Open(const std::string& path);

  // Append one record. When sync is true the record is fsync'd before return,
  // which is what makes the write durable (the caller may then ack it).
  Status AddRecord(ValueType type, const std::string& key,
                   const std::string& value, bool sync);

  Status Close();

 private:
  int fd_ = -1;
  std::string path_;
};

// Replay every intact record in the log at `path`, in order, invoking
// `handler` for each. Stops cleanly at a torn/corrupt tail record. *num_replayed
// receives the count of records applied. A missing file is not an error (fresh
// database) — it replays zero records.
Status WalReplay(
    const std::string& path,
    const std::function<void(ValueType, const std::string&,
                             const std::string&)>& handler,
    size_t* num_replayed);

}  // namespace lsmdb
