#include "lsmdb/skiplist.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <string>
#include <vector>

using lsmdb::LookupResult;
using lsmdb::SkipList;

namespace {

std::string Val(const SkipList& list, const std::string& key) {
  std::string v;
  EXPECT_EQ(list.Get(key, &v), LookupResult::kFound) << "key=" << key;
  return v;
}

}  // namespace

TEST(SkipList, InsertAndLookup) {
  SkipList list;
  list.Put("apple", "red");
  list.Put("banana", "yellow");
  list.Put("cherry", "dark");

  EXPECT_EQ(Val(list, "apple"), "red");
  EXPECT_EQ(Val(list, "banana"), "yellow");
  EXPECT_EQ(Val(list, "cherry"), "dark");
  EXPECT_EQ(list.Size(), 3u);
}

TEST(SkipList, MissingKeyNotFound) {
  SkipList list;
  list.Put("a", "1");
  std::string v = "sentinel";
  EXPECT_EQ(list.Get("zzz", &v), LookupResult::kNotFound);
  EXPECT_EQ(v, "sentinel");  // untouched on miss
}

TEST(SkipList, DuplicateKeyOverwrite) {
  SkipList list;
  list.Put("k", "v1");
  list.Put("k", "v2");
  list.Put("k", "v3");
  EXPECT_EQ(Val(list, "k"), "v3");
  EXPECT_EQ(list.Size(), 1u) << "overwrite must not grow the list";
}

TEST(SkipList, DeleteLeavesTombstone) {
  SkipList list;
  list.Put("k", "v");
  ASSERT_EQ(Val(list, "k"), "v");

  list.Delete("k");
  std::string v;
  EXPECT_EQ(list.Get("k", &v), LookupResult::kDeleted);
  // The key still occupies a slot: the tombstone must survive to shadow older
  // SSTable data on disk.
  EXPECT_EQ(list.Size(), 1u);
}

TEST(SkipList, ReinsertAfterDelete) {
  SkipList list;
  list.Put("k", "v1");
  list.Delete("k");
  EXPECT_EQ(list.Get("k", nullptr), LookupResult::kDeleted);
  list.Put("k", "v2");
  EXPECT_EQ(Val(list, "k"), "v2");
}

TEST(SkipList, IteratorReturnsSortedOrder) {
  SkipList list;
  std::vector<std::string> keys = {"delta", "alpha", "echo", "bravo",
                                   "charlie"};
  for (size_t i = 0; i < keys.size(); ++i) {
    list.Put(keys[i], std::to_string(i));
  }

  std::vector<std::string> seen;
  auto it = list.NewIterator();
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    seen.push_back(it.key());
  }

  std::vector<std::string> expected = keys;
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(seen, expected);
}

TEST(SkipList, IteratorExposesTombstones) {
  SkipList list;
  list.Put("a", "1");
  list.Delete("b");
  list.Put("c", "3");

  auto it = list.NewIterator();
  it.SeekToFirst();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), "a");
  EXPECT_FALSE(it.IsTombstone());
  it.Next();
  EXPECT_EQ(it.key(), "b");
  EXPECT_TRUE(it.IsTombstone());
  it.Next();
  EXPECT_EQ(it.key(), "c");
  EXPECT_FALSE(it.IsTombstone());
  it.Next();
  EXPECT_FALSE(it.Valid());
}

TEST(SkipList, LargeRandomizedSortedInvariant) {
  SkipList list;
  std::mt19937 rng(42);
  std::vector<int> nums(2000);
  for (int i = 0; i < 2000; ++i) nums[i] = i;
  std::shuffle(nums.begin(), nums.end(), rng);

  // Zero-padded keys so lexicographic order matches numeric order.
  auto keyOf = [](int n) {
    char buf[16];
    snprintf(buf, sizeof(buf), "key%08d", n);
    return std::string(buf);
  };

  for (int n : nums) list.Put(keyOf(n), std::to_string(n));
  EXPECT_EQ(list.Size(), 2000u);

  // Every key readable with the right value.
  for (int n : nums) EXPECT_EQ(Val(list, keyOf(n)), std::to_string(n));

  // Iterator visits keys in strictly ascending order.
  auto it = list.NewIterator();
  std::string prev;
  int visited = 0;
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    if (visited > 0) {
      EXPECT_LT(prev, it.key());
    }
    prev = it.key();
    ++visited;
  }
  EXPECT_EQ(visited, 2000);
}

TEST(SkipList, MemoryUsageGrows) {
  SkipList list;
  EXPECT_EQ(list.ApproximateMemoryUsage(), 0u);
  list.Put("some-key", "some-value");
  EXPECT_GT(list.ApproximateMemoryUsage(), 0u);
  size_t before = list.ApproximateMemoryUsage();
  list.Put("another-key", "another-value");
  EXPECT_GT(list.ApproximateMemoryUsage(), before);
}
