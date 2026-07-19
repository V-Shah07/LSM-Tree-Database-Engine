#pragma once

// The database: the public entry point that ties the pieces together. As of
// Phase 3 it is memtable + write-ahead log + crash recovery. Later phases add
// SSTable flushing, bloom filters, compaction, a block cache, and range scans.

#include <memory>
#include <string>

#include "lsmdb/skiplist.h"
#include "lsmdb/status.h"
#include "lsmdb/wal.h"

namespace lsmdb {

struct Options {
  // fsync the WAL on every write before acking. True is the durable default;
  // tests toggle it to measure the cost of durability.
  bool sync = true;
};

class DB {
 public:
  ~DB();

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  // Open (creating if needed) the database rooted at `dir`. Replays the WAL to
  // rebuild the in-memory state before returning.
  static Status Open(const std::string& dir, std::unique_ptr<DB>* out);
  static Status Open(const Options& options, const std::string& dir,
                     std::unique_ptr<DB>* out);

  Status Put(const std::string& key, const std::string& value);
  Status Delete(const std::string& key);

  // kFound (value populated), kDeleted (tombstoned), or kNotFound.
  LookupResult Get(const std::string& key, std::string* value) const;

 private:
  DB(const Options& options, std::string dir);

  Options options_;
  std::string dir_;
  std::string wal_path_;
  std::unique_ptr<SkipList> mem_;
  WalWriter wal_;
};

}  // namespace lsmdb
