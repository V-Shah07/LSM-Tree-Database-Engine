#include "lsmdb/cache.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "lsmdb/sstable.h"

using lsmdb::BlockCache;
using lsmdb::BlockPtr;
using lsmdb::LookupResult;
using lsmdb::SSTableReader;
using lsmdb::SSTableWriter;
using lsmdb::Status;
using lsmdb::ValueType;

namespace {
BlockPtr MakeBlock(const std::string& s) {
  return std::make_shared<const std::string>(s);
}
}  // namespace

TEST(BlockCache, HitAndMissAccounting) {
  BlockCache c(1 << 20);
  EXPECT_EQ(c.Lookup(1, 0), nullptr);  // miss
  c.Insert(1, 0, MakeBlock("hello"));
  ASSERT_NE(c.Lookup(1, 0), nullptr);  // hit
  EXPECT_EQ(*c.Lookup(1, 0), "hello");  // hit

  EXPECT_EQ(c.hits(), 2u);    // two successful lookups after the insert
  EXPECT_EQ(c.misses(), 1u);  // the initial pre-insert lookup
  EXPECT_NEAR(c.HitRate(), 2.0 / 3.0, 1e-9);
}

TEST(BlockCache, EvictsLeastRecentlyUsed) {
  // Capacity for exactly two 100-byte blocks.
  BlockCache c(200);
  std::string blk(100, 'x');
  c.Insert(1, 0, MakeBlock(blk));  // [0]
  c.Insert(1, 1, MakeBlock(blk));  // [1,0]
  // Touch block 0 so block 1 becomes the LRU victim.
  ASSERT_NE(c.Lookup(1, 0), nullptr);  // [0,1]
  c.Insert(1, 2, MakeBlock(blk));      // inserting evicts LRU (block 1)

  EXPECT_NE(c.Lookup(1, 0), nullptr) << "recently used block survived";
  EXPECT_NE(c.Lookup(1, 2), nullptr) << "newest block present";
  EXPECT_EQ(c.Lookup(1, 1), nullptr) << "LRU block evicted";
  EXPECT_LE(c.size_bytes(), c.capacity_bytes());
}

TEST(BlockCache, KeysAreScopedPerFile) {
  BlockCache c(1 << 20);
  c.Insert(1, 0, MakeBlock("from-file-1"));
  c.Insert(2, 0, MakeBlock("from-file-2"));  // same offset, different file
  ASSERT_NE(c.Lookup(1, 0), nullptr);
  ASSERT_NE(c.Lookup(2, 0), nullptr);
  EXPECT_EQ(*c.Lookup(1, 0), "from-file-1");
  EXPECT_EQ(*c.Lookup(2, 0), "from-file-2");
}

TEST(BlockCache, ZeroCapacityIsDisabled) {
  BlockCache c(0);
  c.Insert(1, 0, MakeBlock("x"));
  EXPECT_EQ(c.Lookup(1, 0), nullptr);  // nothing stored
}

// An SSTableReader wired to a cache must serve repeated reads from the cache
// (second read of the same block is a hit) while still returning correct data.
TEST(BlockCache, SSTableReaderPopulatesCache) {
  const std::string path = "/tmp/lsmdb_cache_reader.sst";
  {
    SSTableWriter w(path, /*block_size=*/256);
    ASSERT_TRUE(w.Open().ok());
    for (int i = 0; i < 2000; ++i) {
      char k[16];
      snprintf(k, sizeof(k), "key%06d", i);
      ASSERT_TRUE(w.Add(k, "value" + std::to_string(i), ValueType::kValue).ok());
    }
    ASSERT_TRUE(w.Finish().ok());
  }

  BlockCache cache(1 << 20);
  std::unique_ptr<SSTableReader> r;
  ASSERT_TRUE(SSTableReader::Open(path, &r, &cache, /*file_number=*/7).ok());

  std::string v;
  Status s;
  // First lookup of a key -> cache miss (block read from mmap).
  ASSERT_EQ(r->Get("key000100", &v, &s), LookupResult::kFound);
  EXPECT_EQ(v, "value100");
  uint64_t misses_after_first = cache.misses();
  EXPECT_GE(misses_after_first, 1u);

  // Repeated lookups of the same (and nearby, same-block) keys -> cache hits.
  for (int rep = 0; rep < 50; ++rep) {
    ASSERT_EQ(r->Get("key000100", &v, &s), LookupResult::kFound);
    EXPECT_EQ(v, "value100");
  }
  EXPECT_GT(cache.hits(), 40u);
  EXPECT_EQ(cache.misses(), misses_after_first) << "no new mmap reads on hits";

  ::remove(path.c_str());
}
