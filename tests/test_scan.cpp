#include "lsmdb/db.h"

#include <dirent.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using lsmdb::DB;
using lsmdb::LookupResult;
using lsmdb::Options;

namespace {

std::string Dir(const std::string& tag) {
  return "/tmp/lsmdb_scan_" + std::to_string(::getpid()) + "_" + tag + "_" +
         std::to_string(::rand());
}
std::string KeyOf(int n) {
  char b[16];
  snprintf(b, sizeof(b), "k%06d", n);
  return std::string(b);
}
Options ScanOptions() {
  Options o;
  o.sync = false;
  o.memtable_flush_threshold = 32 * 1024;  // force data onto disk across levels
  o.l0_compaction_trigger = 4;
  o.l1_target_bytes = 128 * 1024;
  o.level_size_multiplier = 4;
  o.target_file_size = 64 * 1024;
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

// Scan must merge memtable + all SSTables into one sorted, deduplicated,
// tombstone-filtered view spanning the requested range.
TEST(Scan, MergesLevelsAndMemtable) {
  std::string dir = Dir("merge");
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(ScanOptions(), dir, &db).ok());

  const int N = 5000;
  for (int i = 0; i < N; ++i) ASSERT_TRUE(db->Put(KeyOf(i), "v" + std::to_string(i)).ok());
  // Overwrite a band with new values (these live in newer L0 / memtable).
  for (int i = 1000; i < 1500; ++i) ASSERT_TRUE(db->Put(KeyOf(i), "NEW" + std::to_string(i)).ok());
  // Delete a band.
  for (int i = 2000; i < 2100; ++i) ASSERT_TRUE(db->Delete(KeyOf(i)).ok());
  db->TEST_WaitForCompactions();  // some data on disk, some still in memtable

  std::vector<std::pair<std::string, std::string>> res;
  ASSERT_TRUE(db->Scan(KeyOf(0), KeyOf(N), &res).ok());

  // Build expectation.
  std::vector<std::pair<std::string, std::string>> expect;
  for (int i = 0; i < N; ++i) {
    if (i >= 2000 && i < 2100) continue;  // deleted
    std::string v = (i >= 1000 && i < 1500) ? "NEW" + std::to_string(i)
                                            : "v" + std::to_string(i);
    expect.emplace_back(KeyOf(i), v);
  }
  ASSERT_EQ(res.size(), expect.size());
  for (size_t i = 0; i < res.size(); ++i) {
    EXPECT_EQ(res[i].first, expect[i].first) << "at " << i;
    EXPECT_EQ(res[i].second, expect[i].second) << "at " << i;
    if (i > 0) {
      EXPECT_LT(res[i - 1].first, res[i].first) << "not sorted at " << i;
    }
  }
  db.reset();
  DestroyDir(dir);
}

TEST(Scan, RespectsBounds) {
  std::string dir = Dir("bounds");
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(ScanOptions(), dir, &db).ok());
  for (int i = 0; i < 1000; ++i) ASSERT_TRUE(db->Put(KeyOf(i), "v").ok());
  db->TEST_FlushMemTable();
  db->TEST_WaitForCompactions();

  std::vector<std::pair<std::string, std::string>> res;
  ASSERT_TRUE(db->Scan(KeyOf(200), KeyOf(300), &res).ok());  // [200, 300)
  ASSERT_EQ(res.size(), 100u);
  EXPECT_EQ(res.front().first, KeyOf(200));
  EXPECT_EQ(res.back().first, KeyOf(299));

  // Unbounded end scans through the last key.
  res.clear();
  ASSERT_TRUE(db->Scan(KeyOf(990), "", &res).ok());
  EXPECT_EQ(res.size(), 10u);
  EXPECT_EQ(res.back().first, KeyOf(999));
  db.reset();
  DestroyDir(dir);
}

TEST(Scan, EmptyRange) {
  std::string dir = Dir("empty");
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(ScanOptions(), dir, &db).ok());
  for (int i = 0; i < 100; ++i) ASSERT_TRUE(db->Put(KeyOf(i), "v").ok());
  std::vector<std::pair<std::string, std::string>> res;
  ASSERT_TRUE(db->Scan("zzz", "zzzz", &res).ok());
  EXPECT_TRUE(res.empty());
  db.reset();
  DestroyDir(dir);
}
