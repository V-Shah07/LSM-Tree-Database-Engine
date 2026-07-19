#pragma once

// A uniform iterator interface over any ordered source of (key, value, type)
// entries -- the memtable or an SSTable. Scan builds a priority-ordered set of
// these and merges them so a range query sees the same newest-wins, tombstone-
// aware view that point lookups do.

#include <string>

#include "lsmdb/status.h"

namespace lsmdb {

class InternalIterator {
 public:
  virtual ~InternalIterator() = default;
  virtual bool Valid() const = 0;
  virtual void Seek(const std::string& target) = 0;  // first key >= target
  virtual void Next() = 0;
  virtual const std::string& key() const = 0;
  virtual const std::string& value() const = 0;
  virtual bool IsTombstone() const = 0;
  virtual Status status() const { return Status::OK(); }
};

}  // namespace lsmdb
