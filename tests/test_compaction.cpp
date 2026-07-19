#include "lsmdb/db.h"

#include <dirent.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <random>
#include <string>
#include <unordered_map>

#include "lsmdb/merging_iterator.h"
#include "lsmdb/sstable.h"

using lsmdb::DB;
using lsmdb::LookupResult;
using lsmdb::Options;
using lsmdb::Status;

namespace {

std::string DirName(const std::string& tag) {
  return "/tmp/lsmdb_compaction_" + std::to_string(::getpid()) + "_" + tag +
         "_" + std::to_string(::rand());
}

std::string KeyOf(int n) {
  char b[24];
  snprintf(b, sizeof(b), "key%09d", n);
  return std::string(b);
}
std::string ValueOf(int n, char tag) {
  // ~200-byte values so a modest key count produces several MB and forces the
  // level cascade at the small thresholds below.
  std::string base = std::to_string(n) + "-" + std::string(1, tag) + "-";
  base.resize(200, 'x');
  return base;
}

// Small thresholds so a few thousand keys cascade through multiple levels.
Options SmallOptions() {
  Options o;
  o.sync = false;  // durability is covered by the Phase 3 crash tests; these
                   // tests exercise compaction correctness, so skip per-write
                   // fsync to keep them fast.
  o.memtable_flush_threshold = 64 * 1024;   // 64 KB memtable
  o.l0_compaction_trigger = 4;
  o.l1_target_bytes = 256 * 1024;           // 256 KB base level
  o.level_size_multiplier = 4;
  o.target_file_size = 128 * 1024;          // 128 KB output files
  o.max_levels = 7;
  return o;
}

void DestroyDir(const std::string& dir) {
  DIR* d = ::opendir(dir.c_str());
  if (d) {
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
      std::string n = e->d_name;
      if (n != "." && n != "..") ::unlink((dir + "/" + n).c_str());
    }
    ::closedir(d);
  }
  ::rmdir(dir.c_str());
}

}  // namespace

// The merging iterator must produce sorted, unique keys with the newest version
// winning (higher-priority child) -- the core compaction invariant.
TEST(MergingIterator, NewestWinsAndDedups) {
  using lsmdb::MergingIterator;
  using lsmdb::SSTableReader;
  using lsmdb::SSTableWriter;
  using lsmdb::ValueType;

  std::string p_new = "/tmp/lsmdb_merge_new.sst";
  std::string p_old = "/tmp/lsmdb_merge_old.sst";
  {
    SSTableWriter w(p_new, 256);
    ASSERT_TRUE(w.Open().ok());
    w.Add("a", "new-a", ValueType::kValue);
    w.Add("c", "", ValueType::kTombstone);  // delete c
    ASSERT_TRUE(w.Finish().ok());
  }
  {
    SSTableWriter w(p_old, 256);
    ASSERT_TRUE(w.Open().ok());
    w.Add("a", "old-a", ValueType::kValue);
    w.Add("b", "old-b", ValueType::kValue);
    w.Add("c", "old-c", ValueType::kValue);
    ASSERT_TRUE(w.Finish().ok());
  }
  std::unique_ptr<SSTableReader> rn, ro;
  ASSERT_TRUE(SSTableReader::Open(p_new, &rn).ok());
  ASSERT_TRUE(SSTableReader::Open(p_old, &ro).ok());

  std::vector<SSTableReader::Iterator> kids;
  kids.push_back(rn->NewIterator());  // priority 0 (newest)
  kids.push_back(ro->NewIterator());
  MergingIterator m(std::move(kids));

  std::vector<std::pair<std::string, std::string>> got;  // key -> value/(TOMB)
  for (m.SeekToFirst(); m.Valid(); m.Next()) {
    got.push_back({m.key(), m.IsTombstone() ? "TOMB" : m.value()});
  }
  ASSERT_EQ(got.size(), 3u);
  EXPECT_EQ(got[0].first, "a"); EXPECT_EQ(got[0].second, "new-a");  // newest won
  EXPECT_EQ(got[1].first, "b"); EXPECT_EQ(got[1].second, "old-b");
  EXPECT_EQ(got[2].first, "c"); EXPECT_EQ(got[2].second, "TOMB");   // delete won
  ::remove(p_new.c_str());
  ::remove(p_old.c_str());
}

// End-to-end: write enough to trigger multiple levels of compaction, then
// verify every live key is still correct and deleted keys are gone.
TEST(Compaction, MultiLevelIntegrity) {
  std::string dir = DirName("integrity");
  const int N = 20000;

  std::unordered_map<std::string, std::string> live;  // expected surviving state
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(SmallOptions(), dir, &db).ok());

    // Round 1: write all N keys.
    for (int i = 0; i < N; ++i) {
      ASSERT_TRUE(db->Put(KeyOf(i), ValueOf(i, 'A')).ok());
      live[KeyOf(i)] = ValueOf(i, 'A');
    }
    // Round 2: overwrite every 3rd key with a new value.
    for (int i = 0; i < N; i += 3) {
      ASSERT_TRUE(db->Put(KeyOf(i), ValueOf(i, 'B')).ok());
      live[KeyOf(i)] = ValueOf(i, 'B');
    }
    // Round 3: delete every 5th key.
    for (int i = 0; i < N; i += 5) {
      ASSERT_TRUE(db->Delete(KeyOf(i)).ok());
      live.erase(KeyOf(i));
    }

    db->TEST_FlushMemTable();
    db->TEST_WaitForCompactions();

    int levels = db->TEST_NumLevelsWithData();
    DB::Stats st = db->GetStats();
    printf("[compaction] levels with data = %d; write-amp = %.2fx; "
           "user=%.1f MB sstable=%.1f MB\n",
           levels, st.write_amplification, st.user_bytes / 1e6,
           st.sstable_bytes_written / 1e6);
    for (size_t l = 0; l < st.files_per_level.size(); ++l) {
      if (st.files_per_level[l] > 0) {
        printf("            L%zu: %d files, %.2f MB\n", l,
               st.files_per_level[l], st.bytes_per_level[l] / 1e6);
      }
    }
    EXPECT_GE(levels, 2) << "expected data spread across >=2 levels";

    // Verify integrity against the expected live set.
    for (int i = 0; i < N; ++i) {
      std::string v;
      LookupResult r = db->Get(KeyOf(i), &v);
      auto it = live.find(KeyOf(i));
      if (it == live.end()) {
        EXPECT_NE(r, LookupResult::kFound) << "deleted key resurfaced: " << i;
      } else {
        ASSERT_EQ(r, LookupResult::kFound) << "missing live key: " << i;
        EXPECT_EQ(v, it->second) << "wrong value at " << i;
      }
    }
  }

  // Reopen from disk (manifest + SSTables) and re-verify -- compaction results
  // are durable, not just in-memory.
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(SmallOptions(), dir, &db).ok());
    for (int i = 0; i < N; i += 7) {  // sample
      std::string v;
      LookupResult r = db->Get(KeyOf(i), &v);
      auto it = live.find(KeyOf(i));
      if (it == live.end()) {
        EXPECT_NE(r, LookupResult::kFound) << "after reopen, deleted: " << i;
      } else {
        ASSERT_EQ(r, LookupResult::kFound) << "after reopen, missing: " << i;
        EXPECT_EQ(v, it->second);
      }
    }
  }
  DestroyDir(dir);
}

// Tombstones must not survive once their key reaches the bottommost level with
// nothing beneath it.
TEST(Compaction, TombstonesDroppedAtBottom) {
  std::string dir = DirName("tomb");
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(SmallOptions(), dir, &db).ok());
    const int N = 12000;
    for (int i = 0; i < N; ++i) ASSERT_TRUE(db->Put(KeyOf(i), ValueOf(i, 'A')).ok());
    for (int i = 0; i < N; ++i) ASSERT_TRUE(db->Delete(KeyOf(i)).ok());
    db->TEST_FlushMemTable();
    db->TEST_WaitForCompactions();

    // Everything deleted -> nothing should be found.
    for (int i = 0; i < N; ++i) {
      std::string v;
      EXPECT_NE(db->Get(KeyOf(i), &v), LookupResult::kFound) << i;
    }
  }
  DestroyDir(dir);
}
