// Phase 4 measurement driver: how much work does the bloom filter save on
// missing-key lookups?
//
// Builds one SSTable, then runs a large batch of lookups for keys that are NOT
// present, once with the bloom filter enabled and once with it bypassed, and
// reports:
//   - data blocks read (the disk-touch metric the filter is meant to cut),
//   - wall-clock time and the resulting speedup,
//   - measured vs theoretical false-positive rate.
//
// Output is committed as the evidence behind the bloom bullet.

#include <cmath>
#include <cstdio>
#include <chrono>
#include <memory>
#include <string>

#include "lsmdb/sstable.h"

using lsmdb::LookupResult;
using lsmdb::SSTableReader;
using lsmdb::SSTableWriter;
using lsmdb::Status;
using lsmdb::ValueType;
using Clock = std::chrono::steady_clock;

// Present keys are the even integers, missing keys the odd ones, under a shared
// prefix. This makes every missing key fall *between* two present keys, so the
// sparse index alone cannot rule it out -- without the bloom filter the reader
// is forced to read and scan a candidate block for each miss. That is exactly
// the wasted I/O the bloom filter is meant to eliminate.
static std::string PresentKey(int n) {
  char b[24];
  snprintf(b, sizeof(b), "key-%012d", 2 * n);
  return std::string(b);
}
static std::string MissingKey(int n) {
  char b[24];
  snprintf(b, sizeof(b), "key-%012d", 2 * n + 1);
  return std::string(b);
}

static double SecondsSince(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

int main() {
  const int kNumKeys = 1000000;   // keys stored in the table
  const int kNumLookups = 1000000;  // missing-key lookups per pass
  const int kBitsPerKey = 10;
  const size_t kBlockSize = 4096;
  const std::string path = "/tmp/lsmdb_bench_bloom.sst";

  // ---- build ----
  {
    SSTableWriter w(path, kBlockSize, kBitsPerKey);
    Status s = w.Open();
    if (!s.ok()) { fprintf(stderr, "open: %s\n", s.ToString().c_str()); return 1; }
    for (int i = 0; i < kNumKeys; ++i) {
      s = w.Add(PresentKey(i), "v", ValueType::kValue);
      if (!s.ok()) { fprintf(stderr, "add: %s\n", s.ToString().c_str()); return 1; }
    }
    s = w.Finish();
    if (!s.ok()) { fprintf(stderr, "finish: %s\n", s.ToString().c_str()); return 1; }
  }

  std::unique_ptr<SSTableReader> r;
  Status s = SSTableReader::Open(path, &r);
  if (!s.ok()) { fprintf(stderr, "reader open: %s\n", s.ToString().c_str()); return 1; }

  // Warm the page cache so we compare CPU/lookup work, not first-touch faults.
  {
    std::string v; Status st;
    for (int i = 0; i < kNumLookups; ++i) r->Get(MissingKey(i), &v, &st);
  }

  // ---- pass A: bloom DISABLED (must consult the index + scan a block) ----
  r->SetFilterEnabled(false);
  r->ResetStats();
  auto t0 = Clock::now();
  long found_a = 0;
  {
    std::string v; Status st;
    for (int i = 0; i < kNumLookups; ++i) {
      if (r->Get(MissingKey(i), &v, &st) == LookupResult::kFound) ++found_a;
    }
  }
  double secs_a = SecondsSince(t0);
  uint64_t blocks_a = r->blocks_read();

  // ---- pass B: bloom ENABLED (short-circuits misses) ----
  r->SetFilterEnabled(true);
  r->ResetStats();
  t0 = Clock::now();
  long found_b = 0;
  {
    std::string v; Status st;
    for (int i = 0; i < kNumLookups; ++i) {
      if (r->Get(MissingKey(i), &v, &st) == LookupResult::kFound) ++found_b;
    }
  }
  double secs_b = SecondsSince(t0);
  uint64_t blocks_b = r->blocks_read();

  // blocks_b are exactly the filter's false positives (each passed the filter
  // and then scanned one block without finding the key).
  double measured_fp = static_cast<double>(blocks_b) / kNumLookups;
  double ratio = static_cast<double>(r->filter().num_probes()) / kBitsPerKey;
  double theoretical_fp = std::pow(1.0 - std::exp(-ratio), r->filter().num_probes());

  double block_reduction = 100.0 * (1.0 - static_cast<double>(blocks_b) /
                                              static_cast<double>(blocks_a));
  double speedup = secs_a / secs_b;

  printf("==== Phase 4: bloom filter on missing-key lookups ====\n");
  printf("table keys        : %d\n", kNumKeys);
  printf("missing lookups   : %d per pass\n", kNumLookups);
  printf("bits/key, k       : %d, %d\n", kBitsPerKey, r->filter().num_probes());
  printf("filter size       : %u bits (%.2f MB)\n", r->filter().num_bits(),
         r->filter().num_bits() / 8.0 / 1e6);
  printf("\n");
  printf("                     data blocks read   wall time     ns/lookup\n");
  printf("bloom DISABLED     : %14llu   %8.3f s   %8.1f\n",
         (unsigned long long)blocks_a, secs_a, secs_a / kNumLookups * 1e9);
  printf("bloom ENABLED      : %14llu   %8.3f s   %8.1f\n",
         (unsigned long long)blocks_b, secs_b, secs_b / kNumLookups * 1e9);
  printf("\n");
  printf("disk block reads avoided : %.2f%%  (%llu -> %llu)\n", block_reduction,
         (unsigned long long)blocks_a, (unsigned long long)blocks_b);
  printf("lookup speedup           : %.2fx\n", speedup);
  printf("false-positive rate      : measured %.4f  vs theoretical %.4f\n",
         measured_fp, theoretical_fp);
  printf("sanity (spurious founds) : %ld  (must be 0)\n", found_a + found_b);

  std::remove(path.c_str());
  return 0;
}
