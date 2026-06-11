// Phase 6 measurement driver: LRU block cache under a skewed read workload.
//
// Loads a dataset, flushes it to SSTables, then replays the same skewed
// (hot-key) read workload twice against the on-disk data: once with the block
// cache disabled and once enabled. Reports read-latency percentiles, the cache
// hit rate, and the latency improvement. Committed as the cache-bullet evidence.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "lsmdb/db.h"

using lsmdb::DB;
using lsmdb::LookupResult;
using lsmdb::Options;
using lsmdb::Status;
using Clock = std::chrono::steady_clock;

static std::string KeyOf(int n) {
  char b[24];
  snprintf(b, sizeof(b), "key%09d", n);
  return std::string(b);
}
static std::string ValueOf(int n) {
  std::string s = std::to_string(n) + "-";
  s.resize(96, 'x');
  return s;
}

struct Result {
  double mean_ns, p50_ns, p99_ns;
  double hit_rate;
};

static Result RunWorkload(const std::string& dir, size_t cache_bytes, int N,
                          int reads) {
  Options o;
  o.sync = false;
  o.block_cache_bytes = cache_bytes;
  std::unique_ptr<DB> db;
  Status s = DB::Open(o, dir, &db);
  if (!s.ok()) { fprintf(stderr, "open: %s\n", s.ToString().c_str()); exit(1); }

  // Skewed workload: 90% of reads target the hottest 1% of keys.
  std::mt19937 rng(2026);
  std::uniform_real_distribution<double> coin(0.0, 1.0);
  const int hot = std::max(1, N / 100);
  std::uniform_int_distribution<int> hot_pick(0, hot - 1);
  std::uniform_int_distribution<int> any_pick(0, N - 1);

  auto pick = [&]() { return coin(rng) < 0.90 ? hot_pick(rng) : any_pick(rng); };

  // Warm-up: touch the working set so we compare steady-state, and (with cache
  // on) let the hot blocks populate the cache.
  std::string v;
  for (int i = 0; i < reads / 2; ++i) db->Get(KeyOf(pick()), &v);

  DB::Stats before = db->GetStats();
  uint64_t base_hits = before.cache_hits, base_misses = before.cache_misses;

  std::vector<double> lat;
  lat.reserve(reads);
  for (int i = 0; i < reads; ++i) {
    std::string val;
    std::string k = KeyOf(pick());
    auto t0 = Clock::now();
    db->Get(k, &val);
    lat.push_back(std::chrono::duration<double, std::nano>(Clock::now() - t0).count());
  }

  DB::Stats after = db->GetStats();
  std::sort(lat.begin(), lat.end());
  double sum = 0;
  for (double x : lat) sum += x;

  Result r;
  r.mean_ns = sum / lat.size();
  r.p50_ns = lat[lat.size() * 50 / 100];
  r.p99_ns = lat[lat.size() * 99 / 100];
  uint64_t h = after.cache_hits - base_hits;
  uint64_t m = after.cache_misses - base_misses;
  r.hit_rate = (h + m) ? static_cast<double>(h) / (h + m) : 0.0;
  return r;
}

int main() {
  const std::string dir = "/tmp/lsmdb_bench_cache";
  if (system(("rm -rf " + dir).c_str()) != 0) { /* best effort */ }

  const int N = 200000;
  const int reads = 500000;

  // Build + flush the dataset to disk once.
  {
    Options o;
    o.sync = false;
    o.memtable_flush_threshold = 1 * 1024 * 1024;
    std::unique_ptr<DB> db;
    Status s = DB::Open(o, dir, &db);
    if (!s.ok()) { fprintf(stderr, "build open: %s\n", s.ToString().c_str()); return 1; }
    for (int i = 0; i < N; ++i) db->Put(KeyOf(i), ValueOf(i));
    db->TEST_FlushMemTable();
    db->TEST_WaitForCompactions();
  }

  Result no_cache = RunWorkload(dir, /*cache_bytes=*/0, N, reads);
  Result with_cache = RunWorkload(dir, /*cache_bytes=*/16 * 1024 * 1024, N, reads);

  printf("==== Phase 6: LRU block cache under a skewed workload ====\n");
  printf("dataset            : %d keys, ~%.1f MB values\n", N, N * 96 / 1e6);
  printf("workload           : %d reads, 90%% into hottest 1%% of keys\n", reads);
  printf("cache size         : 16 MB\n\n");
  printf("                     mean       p50        p99       cache hit rate\n");
  printf("cache DISABLED     : %7.0f ns %7.0f ns %8.0f ns    %5.1f%%\n",
         no_cache.mean_ns, no_cache.p50_ns, no_cache.p99_ns,
         no_cache.hit_rate * 100);
  printf("cache ENABLED      : %7.0f ns %7.0f ns %8.0f ns    %5.1f%%\n",
         with_cache.mean_ns, with_cache.p50_ns, with_cache.p99_ns,
         with_cache.hit_rate * 100);
  printf("\n");
  printf("mean read latency improvement : %.2fx\n",
         no_cache.mean_ns / with_cache.mean_ns);
  printf("p99  read latency improvement : %.2fx\n",
         no_cache.p99_ns / with_cache.p99_ns);
  printf("cache hit rate                : %.1f%%\n", with_cache.hit_rate * 100);

  if (system(("rm -rf " + dir).c_str()) != 0) { /* best effort */ }
  return 0;
}
