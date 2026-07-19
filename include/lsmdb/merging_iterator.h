#pragma once

// K-way merge over several sorted SSTable iterators, producing one globally
// sorted stream of *unique* user keys. This is the heart of compaction.
//
// The children are supplied in priority order (index 0 = newest / highest
// priority). When the same user key appears in multiple children, the value
// from the highest-priority child wins and the stale copies are discarded --
// that is how compaction collapses overwrites and lets a newer tombstone shadow
// an older value.
//
// A linear scan over children is used rather than a heap: the number of inputs
// to a single compaction is small (a handful of files), so the constant factor
// beats the bookkeeping, and the logic is easier to verify correct.

#include <string>
#include <vector>

#include "lsmdb/sstable.h"
#include "lsmdb/status.h"

namespace lsmdb {

class MergingIterator {
 public:
  // `children` must already be ordered by priority (newest first).
  explicit MergingIterator(std::vector<SSTableReader::Iterator> children);

  void SeekToFirst();
  bool Valid() const { return valid_; }
  void Next();

  const std::string& key() const { return cur_key_; }
  const std::string& value() const { return cur_value_; }
  bool IsTombstone() const { return cur_is_tombstone_; }
  Status status() const { return status_; }

 private:
  // Emit the next unique key from the current child heads and advance every
  // child positioned on that key.
  void Advance();

  std::vector<SSTableReader::Iterator> children_;
  std::string cur_key_;
  std::string cur_value_;
  bool cur_is_tombstone_ = false;
  bool valid_ = false;
  Status status_;
};

}  // namespace lsmdb
