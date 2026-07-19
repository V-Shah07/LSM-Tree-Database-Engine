#pragma once

// A skip list memtable, implemented from scratch (no std::map).
//
// This is the in-memory write buffer of the LSM tree. Every Put/Delete lands
// here first (after the WAL). Keys are kept in sorted order so the whole
// structure can be flushed to an immutable SSTable as a single sequential
// sorted run.
//
// Deletes are stored as tombstones rather than removing the node: an older
// value for the same key may still live in an SSTable on disk, and the
// tombstone is what shadows it until compaction reclaims the space.

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace lsmdb {

// Result of a point lookup in the memtable.
//   kFound     -> key present, *value populated with the live value
//   kDeleted   -> key present but tombstoned (a delete shadows older data)
//   kNotFound  -> key absent from this memtable (may exist in an SSTable)
enum class LookupResult { kFound, kDeleted, kNotFound };

class SkipList {
 public:
  explicit SkipList(unsigned seed = 0xC0FFEE);
  ~SkipList();

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // Insert or overwrite a live value for key.
  void Put(const std::string& key, const std::string& value);

  // Insert a tombstone for key (logical delete).
  void Delete(const std::string& key);

  // Point lookup. See LookupResult for the meaning of the return value.
  LookupResult Get(const std::string& key, std::string* value) const;

  // Number of distinct keys currently held (tombstones included).
  size_t Size() const { return count_; }

  // Rough heap footprint of the stored keys/values, used to decide when the
  // memtable is full and should be flushed to disk.
  size_t ApproximateMemoryUsage() const { return mem_usage_; }

  // Forward iterator over all entries in ascending key order. Used by the
  // SSTable writer to stream the memtable out as a sorted run.
  class Iterator {
   public:
    explicit Iterator(const SkipList* list);
    bool Valid() const { return node_ != nullptr; }
    void SeekToFirst();
    void Seek(const std::string& target);  // first key >= target
    void Next();
    const std::string& key() const;
    const std::string& value() const;
    bool IsTombstone() const;

   private:
    const SkipList* list_;
    void* node_;  // erased SkipList::Node*, kept opaque in the public header
  };

  Iterator NewIterator() const { return Iterator(this); }

 private:
  static constexpr int kMaxLevel = 16;

  struct Node {
    std::string key;
    std::string value;
    bool is_tombstone = false;
    // forward[i] is the next node at level i. Length == this node's height.
    std::vector<Node*> forward;
    explicit Node(int height) : forward(height, nullptr) {}
    Node(std::string k, std::string v, bool tomb, int height)
        : key(std::move(k)),
          value(std::move(v)),
          is_tombstone(tomb),
          forward(height, nullptr) {}
  };

  int RandomLevel();
  // Shared insert path for both Put (tombstone=false) and Delete (true).
  void Insert(const std::string& key, const std::string& value, bool tombstone);

  Node* head_;
  int level_;         // current highest populated level (1-based count)
  size_t count_;
  size_t mem_usage_;

  mutable std::mt19937 rng_;
  std::uniform_real_distribution<double> dist_;

  friend class Iterator;
};

}  // namespace lsmdb
