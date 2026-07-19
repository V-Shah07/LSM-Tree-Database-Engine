#pragma once

// LRU block cache. Frequently-read SSTable blocks are kept decoded and
// CRC-verified in memory, keyed by (file number, block offset). A point lookup
// that hits the cache skips both the block copy out of the mmap and the CRC
// recomputation, which is the bulk of the per-lookup CPU cost. Under a skewed
// (hot-key) workload the cache absorbs the large majority of reads.
//
// Implementation is a classic from-scratch LRU: a doubly-linked recency list
// (most-recently-used at the front) plus a hash map from key to list position,
// giving O(1) lookup, insert, and eviction. Capacity is a byte budget; the
// least-recently-used blocks are evicted when it is exceeded.

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace lsmdb {

// A shared, immutable block of bytes. shared_ptr lets a cached block be returned
// to a reader without copying while the cache (and any in-flight reader) keep it
// alive.
using BlockPtr = std::shared_ptr<const std::string>;

class BlockCache {
 public:
  explicit BlockCache(size_t capacity_bytes)
      : capacity_bytes_(capacity_bytes) {}

  // Return the cached block for (file_number, offset), or nullptr on a miss.
  // A hit moves the block to most-recently-used.
  BlockPtr Lookup(uint64_t file_number, uint64_t offset);

  // Insert (or refresh) a block, evicting LRU entries to stay within budget.
  void Insert(uint64_t file_number, uint64_t offset, BlockPtr block);

  uint64_t hits() const;
  uint64_t misses() const;
  double HitRate() const;
  size_t size_bytes() const;
  size_t capacity_bytes() const { return capacity_bytes_; }
  void ResetStats();

 private:
  struct Key {
    uint64_t file;
    uint64_t offset;
    bool operator==(const Key& o) const {
      return file == o.file && offset == o.offset;
    }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const {
      // Mix the two 64-bit fields; block offsets are already well spread.
      return std::hash<uint64_t>()(k.file * 1099511628211ull ^ k.offset);
    }
  };
  struct Entry {
    Key key;
    BlockPtr block;
    size_t bytes;
  };

  mutable std::mutex mu_;
  size_t capacity_bytes_;
  size_t used_bytes_ = 0;
  std::list<Entry> lru_;  // front = most recently used
  std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> map_;
  uint64_t hits_ = 0;
  uint64_t misses_ = 0;
};

}  // namespace lsmdb
