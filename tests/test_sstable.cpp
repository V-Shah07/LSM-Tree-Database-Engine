#include "lsmdb/sstable.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "lsmdb/skiplist.h"

using lsmdb::FlushMemTableToSSTable;
using lsmdb::LookupResult;
using lsmdb::SkipList;
using lsmdb::SSTableReader;
using lsmdb::SSTableWriter;
using lsmdb::Status;
using lsmdb::ValueType;

namespace {

// Each test writes to a unique temp path and cleans it up.
class SSTableTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = "/tmp/lsmdb_sst_" + std::to_string(::getpid()) + "_" +
            std::to_string(counter_++) + ".sst";
  }
  void TearDown() override { std::remove(path_.c_str()); }
  std::string path_;
  static int counter_;
};
int SSTableTest::counter_ = 0;

std::string KeyOf(int n) {
  char buf[16];
  snprintf(buf, sizeof(buf), "key%08d", n);
  return std::string(buf);
}

}  // namespace

TEST_F(SSTableTest, WriteReadEveryKey) {
  // Small block size forces many blocks so the sparse index and binary search
  // are actually exercised, not a single-block degenerate case.
  SSTableWriter w(path_, /*block_size=*/256);
  ASSERT_TRUE(w.Open().ok());
  const int N = 5000;
  for (int i = 0; i < N; ++i) {
    ASSERT_TRUE(w.Add(KeyOf(i), "val" + std::to_string(i), ValueType::kValue).ok());
  }
  ASSERT_TRUE(w.Finish().ok());

  std::unique_ptr<SSTableReader> r;
  ASSERT_TRUE(SSTableReader::Open(path_, &r).ok());
  EXPECT_EQ(r->num_entries(), static_cast<uint64_t>(N));
  EXPECT_EQ(r->smallest_key(), KeyOf(0));
  EXPECT_EQ(r->largest_key(), KeyOf(N - 1));

  for (int i = 0; i < N; ++i) {
    std::string v;
    Status s;
    EXPECT_EQ(r->Get(KeyOf(i), &v, &s), LookupResult::kFound) << "i=" << i;
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(v, "val" + std::to_string(i));
  }
}

TEST_F(SSTableTest, MissingKeysReportNotFound) {
  SSTableWriter w(path_, 256);
  ASSERT_TRUE(w.Open().ok());
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(w.Add(KeyOf(i * 2), "v", ValueType::kValue).ok());  // evens only
  }
  ASSERT_TRUE(w.Finish().ok());

  std::unique_ptr<SSTableReader> r;
  ASSERT_TRUE(SSTableReader::Open(path_, &r).ok());
  std::string v;
  Status s;
  EXPECT_EQ(r->Get(KeyOf(1), &v, &s), LookupResult::kNotFound);   // odd, interior
  EXPECT_EQ(r->Get("aaa_before_all", &v, &s), LookupResult::kNotFound);
  EXPECT_EQ(r->Get("zzz_after_all", &v, &s), LookupResult::kNotFound);
}

TEST_F(SSTableTest, TombstonesReadBackAsDeleted) {
  SSTableWriter w(path_, 256);
  ASSERT_TRUE(w.Open().ok());
  ASSERT_TRUE(w.Add("a", "1", ValueType::kValue).ok());
  ASSERT_TRUE(w.Add("b", "", ValueType::kTombstone).ok());
  ASSERT_TRUE(w.Add("c", "3", ValueType::kValue).ok());
  ASSERT_TRUE(w.Finish().ok());

  std::unique_ptr<SSTableReader> r;
  ASSERT_TRUE(SSTableReader::Open(path_, &r).ok());
  std::string v;
  Status s;
  EXPECT_EQ(r->Get("a", &v, &s), LookupResult::kFound);
  EXPECT_EQ(v, "1");
  EXPECT_EQ(r->Get("b", &v, &s), LookupResult::kDeleted);
  EXPECT_EQ(r->Get("c", &v, &s), LookupResult::kFound);
}

TEST_F(SSTableTest, IteratorYieldsSortedRecords) {
  SSTableWriter w(path_, 128);
  ASSERT_TRUE(w.Open().ok());
  const int N = 1000;
  for (int i = 0; i < N; ++i) {
    ValueType t = (i % 5 == 0) ? ValueType::kTombstone : ValueType::kValue;
    ASSERT_TRUE(w.Add(KeyOf(i), t == ValueType::kValue ? "v" : "", t).ok());
  }
  ASSERT_TRUE(w.Finish().ok());

  std::unique_ptr<SSTableReader> r;
  ASSERT_TRUE(SSTableReader::Open(path_, &r).ok());
  auto it = r->NewIterator();
  int count = 0;
  std::string prev;
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    if (count > 0) {
      EXPECT_LT(prev, it.key());
    }
    prev = it.key();
    EXPECT_EQ(it.IsTombstone(), (count % 5 == 0));
    ++count;
  }
  ASSERT_TRUE(it.status().ok());
  EXPECT_EQ(count, N);
}

TEST_F(SSTableTest, FlushFromMemtable) {
  SkipList mem;
  mem.Put("delta", "4");
  mem.Put("alpha", "1");
  mem.Delete("charlie");
  mem.Put("bravo", "2");

  uint64_t sz = 0, n = 0;
  ASSERT_TRUE(FlushMemTableToSSTable(mem, path_, &sz, &n).ok());
  EXPECT_EQ(n, 4u);
  EXPECT_GT(sz, 0u);

  std::unique_ptr<SSTableReader> r;
  ASSERT_TRUE(SSTableReader::Open(path_, &r).ok());
  std::string v;
  Status s;
  EXPECT_EQ(r->Get("alpha", &v, &s), LookupResult::kFound);
  EXPECT_EQ(v, "1");
  EXPECT_EQ(r->Get("charlie", &v, &s), LookupResult::kDeleted);
  EXPECT_EQ(r->smallest_key(), "alpha");
  EXPECT_EQ(r->largest_key(), "delta");
}

// Corrupting a single byte inside a data block must be caught by the block CRC
// rather than silently returning wrong data.
TEST_F(SSTableTest, CorruptionCaughtByBlockCrc) {
  SSTableWriter w(path_, 256);
  ASSERT_TRUE(w.Open().ok());
  const int N = 2000;
  for (int i = 0; i < N; ++i) {
    ASSERT_TRUE(w.Add(KeyOf(i), "value" + std::to_string(i), ValueType::kValue).ok());
  }
  ASSERT_TRUE(w.Finish().ok());

  // Flip a byte early in the file (inside the first data block, well before the
  // index/footer).
  {
    std::fstream f(path_, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(f.good());
    f.seekg(40);
    char c;
    f.read(&c, 1);
    c ^= 0xFF;
    f.seekp(40);
    f.write(&c, 1);
    f.close();
  }

  std::unique_ptr<SSTableReader> r;
  ASSERT_TRUE(SSTableReader::Open(path_, &r).ok());  // index/footer still intact
  // A key living in the corrupted first block must surface a Corruption status
  // instead of a wrong value.
  std::string v;
  Status s;
  LookupResult res = r->Get(KeyOf(0), &v, &s);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_NE(res, LookupResult::kFound);
}

TEST_F(SSTableTest, CorruptMagicRejectedOnOpen) {
  SSTableWriter w(path_, 256);
  ASSERT_TRUE(w.Open().ok());
  ASSERT_TRUE(w.Add("k", "v", ValueType::kValue).ok());
  ASSERT_TRUE(w.Finish().ok());

  // Clobber the 4-byte magic at the very end of the footer.
  {
    std::fstream f(path_, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(-1, std::ios::end);
    char bad = 0x00;
    f.write(&bad, 1);
    f.close();
  }
  std::unique_ptr<SSTableReader> r;
  Status s = SSTableReader::Open(path_, &r);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}
