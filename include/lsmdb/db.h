#pragma once

// The database. As of Phase 5 it is a full (if compact) LSM tree:
//
//   writes -> WAL (durable) -> memtable (skip list)
//   memtable fills -> flushed to an immutable L0 SSTable
//   a level fills -> background thread compacts it into the next level
//
// Reads consult the memtable, then L0 newest-first, then each deeper level
// (which is kept non-overlapping so at most one file per level can hold a key),
// stopping at the first hit. Bloom filters keep most of those probes off disk.
//
// Levels ≥1 are maintained as non-overlapping sorted runs by always merging the
// selected input with exactly the overlapping files in the next level, which is
// what keeps write amplification bounded and measurable.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lsmdb/skiplist.h"
#include "lsmdb/sstable.h"
#include "lsmdb/status.h"
#include "lsmdb/wal.h"

namespace lsmdb {

struct Options {
  bool sync = true;                                  // fsync WAL before ack
  size_t memtable_flush_threshold = 4 * 1024 * 1024; // flush memtable at 4 MB
  int l0_compaction_trigger = 4;                     // L0 files before compaction
  size_t l1_target_bytes = 8 * 1024 * 1024;          // byte budget of level 1
  int level_size_multiplier = 10;                    // each level ~10x the prior
  size_t target_file_size = 2 * 1024 * 1024;         // split outputs at 2 MB
  int max_levels = 7;
  int bloom_bits_per_key = 10;
};

class DB {
 public:
  struct Stats {
    uint64_t user_bytes = 0;             // key+value bytes the caller wrote
    uint64_t flush_bytes = 0;            // bytes written by memtable flushes
    uint64_t compaction_bytes = 0;       // bytes written by compactions
    uint64_t sstable_bytes_written = 0;  // flush + compaction
    double write_amplification = 0.0;    // sstable_bytes_written / user_bytes
    std::vector<int> files_per_level;
    std::vector<uint64_t> bytes_per_level;
  };

  ~DB();
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  static Status Open(const std::string& dir, std::unique_ptr<DB>* out);
  static Status Open(const Options& options, const std::string& dir,
                     std::unique_ptr<DB>* out);

  Status Put(const std::string& key, const std::string& value);
  Status Delete(const std::string& key);
  LookupResult Get(const std::string& key, std::string* value) const;

  Stats GetStats() const;

  // Test/utility hooks used by the compaction benchmark to drive the engine
  // deterministically.
  Status TEST_FlushMemTable();       // force the current memtable out to L0
  void TEST_WaitForCompactions();    // block until the background thread is idle
  int TEST_NumLevelsWithData() const;

 private:
  // ---- on-disk file bookkeeping ----
  struct FileMeta {
    uint64_t number = 0;
    std::string smallest;
    std::string largest;
    uint64_t file_size = 0;
    uint64_t num_entries = 0;
    std::shared_ptr<SSTableReader> reader;
  };
  using FileMetaPtr = std::shared_ptr<FileMeta>;

  // An immutable snapshot of the level layout. Swapped atomically under mu_;
  // readers hold a shared_ptr so a concurrent compaction can't pull files out
  // from under them.
  struct Version {
    std::vector<std::vector<FileMetaPtr>> levels;  // levels[0] == L0
  };
  using VersionPtr = std::shared_ptr<Version>;

  struct Compaction {
    int source_level = 0;
    int target_level = 0;
    bool bottommost = false;
    std::vector<FileMetaPtr> inputs;  // in priority order (newest first)
  };

  DB(const Options& options, std::string dir);

  std::string FilePath(uint64_t number) const;
  std::string ManifestPath() const;

  // Recovery / persistence.
  Status Recover();
  Status LoadManifest();
  Status WriteManifest(const VersionPtr& v);
  void RemoveOrphanFiles(const VersionPtr& v);

  // Flush + compaction (all assume caller coordination via mu_ where noted).
  Status FlushMemTable();  // build an L0 file from mem_ and install it
  bool NeedsCompaction(const VersionPtr& v) const;
  bool PickCompaction(const VersionPtr& v, Compaction* c) const;
  Status DoCompaction(const Compaction& c);
  VersionPtr InstallCompaction(const Compaction& c,
                               const std::vector<FileMetaPtr>& outputs) const;
  void MaybeScheduleCompaction();
  void BackgroundLoop();

  LookupResult GetFromVersion(const VersionPtr& v, const std::string& key,
                              std::string* value) const;

  Options options_;
  std::string dir_;
  std::string wal_path_;

  std::unique_ptr<SkipList> mem_;
  WalWriter wal_;

  mutable std::mutex mu_;
  std::condition_variable bg_cv_;    // wakes the background thread
  std::condition_variable idle_cv_;  // notifies when background work drains
  VersionPtr current_;
  uint64_t next_file_number_ = 1;

  std::thread bg_thread_;
  bool bg_busy_ = false;
  bool shutting_down_ = false;

  std::atomic<uint64_t> user_bytes_{0};
  std::atomic<uint64_t> flush_bytes_{0};
  std::atomic<uint64_t> compaction_bytes_{0};
};

}  // namespace lsmdb
