#pragma once

// Immutable on-disk sorted string table (SSTable) in a custom binary format.
//
// File layout (all integers little-endian):
//
//   [ data block 0 ][ crc32 ]
//   [ data block 1 ][ crc32 ]
//   ...
//   [ filter block ][ crc32 ]   (bloom filter over every key)
//   [ index block  ][ crc32 ]
//   [ footer (fixed 44 bytes) ]
//
// A data block is a run of records:
//   u8  type (0=value, 1=tombstone)
//   u32 key_len
//   u32 value_len
//   key bytes
//   value bytes
// Records inside a block, and blocks within the file, are in ascending key
// order. Each block is capped near a target size so it can be checksummed and
// cached as a unit.
//
// The index is *sparse*: one entry per data block giving the block's first key
// and its (offset, length). A point lookup binary-searches the index to find
// the one block that could hold the key, then scans just that block. This is
// what gives O(log n) point lookups without scanning the whole file.
//
// The footer (at a fixed offset from EOF) points at the filter and index:
//   u64 filter_offset
//   u64 filter_length     (bytes of filter data, excluding its trailing crc)
//   u64 index_offset
//   u64 index_length      (bytes of index data, excluding its trailing crc)
//   u64 num_entries
//   u32 magic (0x4C534D42 = "LSMB")

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lsmdb/bloom.h"
#include "lsmdb/cache.h"     // BlockCache, BlockPtr
#include "lsmdb/skiplist.h"  // LookupResult
#include "lsmdb/status.h"

namespace lsmdb {

enum class ValueType : uint8_t { kValue = 0, kTombstone = 1 };

constexpr uint32_t kSSTableMagic = 0x4C534D42;      // "LSMB"
constexpr size_t kFooterSize = 8 + 8 + 8 + 8 + 8 + 4;  // 44 bytes
constexpr int kDefaultBloomBitsPerKey = 10;

// ---- Writer -----------------------------------------------------------------
// Records must be Add()ed in ascending key order (the memtable iterator already
// yields them sorted). Finish() flushes the tail block, writes the index and
// footer, and fsyncs.
class SSTableWriter {
 public:
  explicit SSTableWriter(std::string path, size_t block_size = 4096,
                         int bloom_bits_per_key = kDefaultBloomBitsPerKey);
  ~SSTableWriter();

  Status Open();
  Status Add(const std::string& key, const std::string& value, ValueType type);
  Status Finish();

  uint64_t FileSize() const { return offset_; }
  uint64_t NumEntries() const { return num_entries_; }

 private:
  Status FlushBlock();
  Status WriteRaw(const char* data, size_t n);

  struct IndexEntry {
    std::string first_key;
    uint64_t offset;
    uint32_t length;
  };

  std::string path_;
  size_t block_size_;
  int fd_ = -1;
  uint64_t offset_ = 0;  // bytes written so far == next block's offset
  uint64_t num_entries_ = 0;

  std::string block_buf_;
  std::string block_first_key_;
  std::vector<IndexEntry> index_;
  BloomBuilder bloom_;
  bool finished_ = false;
};

// Build an SSTable directly from a memtable's sorted iterator (the flush path).
Status FlushMemTableToSSTable(const SkipList& mem, const std::string& path,
                              uint64_t* file_size, uint64_t* num_entries);

// ---- Reader -----------------------------------------------------------------
// Opens the file via mmap, loads the (small) sparse index into memory, and
// serves point lookups and full ordered scans. Every block is CRC-verified on
// read.
class SSTableReader {
 public:
  ~SSTableReader();

  // `cache` (optional) and `file_number` enable the shared LRU block cache for
  // point lookups; pass nullptr to read straight from the mmap every time.
  static Status Open(const std::string& path,
                     std::unique_ptr<SSTableReader>* out,
                     BlockCache* cache = nullptr, uint64_t file_number = 0);

  // Point lookup. kFound/kDeleted mean the key is resolved by *this* table;
  // kNotFound means the caller should consult older tables.
  LookupResult Get(const std::string& key, std::string* value,
                   Status* status) const;

  const std::string& smallest_key() const { return smallest_key_; }
  const std::string& largest_key() const { return largest_key_; }
  uint64_t num_entries() const { return num_entries_; }

  // Whether this table carries a bloom filter, and access for measurement.
  bool has_filter() const { return filter_.valid(); }
  const BloomFilter& filter() const { return filter_; }

  // Number of data blocks copied+CRC-verified since Open (or the last
  // ResetStats). The bloom filter's job is to keep this near zero for
  // missing-key lookups; the Phase 4 benchmark reads it directly.
  uint64_t blocks_read() const { return blocks_read_; }
  void ResetStats() const { blocks_read_ = 0; }

  // Toggle the bloom short-circuit. Used only by the benchmark to measure the
  // with/without-filter difference; production reads always leave it on.
  void SetFilterEnabled(bool on) const { filter_enabled_ = on; }

  // Ordered iterator over every record (values and tombstones). Used by
  // compaction and range scans in later phases.
  class Iterator {
   public:
    explicit Iterator(const SSTableReader* reader);
    bool Valid() const { return valid_; }
    Status status() const { return status_; }
    void SeekToFirst();
    void Seek(const std::string& target);  // first key >= target
    void Next();
    const std::string& key() const { return key_; }
    const std::string& value() const { return value_; }
    bool IsTombstone() const { return type_ == ValueType::kTombstone; }

   private:
    void LoadBlock(size_t block_idx);
    void ParseCurrent();

    const SSTableReader* reader_;
    size_t block_idx_ = 0;
    size_t pos_ = 0;             // byte offset within current block
    std::string block_;         // CRC-verified copy of current block data
    bool valid_ = false;
    std::string key_;
    std::string value_;
    ValueType type_ = ValueType::kValue;
    Status status_;
  };

  Iterator NewIterator() const { return Iterator(this); }

 private:
  struct IndexEntry {
    std::string first_key;
    uint64_t offset;
    uint32_t length;
  };

  SSTableReader() = default;

  // Copy block block_idx's data out of the mmap, verifying its CRC.
  Status ReadBlock(size_t block_idx, std::string* out) const;
  // Cache-aware block fetch used by point lookups: consults the LRU cache and
  // populates it on a miss.
  Status GetBlock(size_t block_idx, BlockPtr* out) const;

  const char* base_ = nullptr;
  size_t file_size_ = 0;
  std::vector<IndexEntry> index_;
  BloomFilter filter_;
  BlockCache* cache_ = nullptr;
  uint64_t file_number_ = 0;
  uint64_t num_entries_ = 0;
  std::string smallest_key_;
  std::string largest_key_;

  mutable uint64_t blocks_read_ = 0;
  mutable bool filter_enabled_ = true;
};

}  // namespace lsmdb
