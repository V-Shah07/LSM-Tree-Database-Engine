// Google Benchmark microbenchmarks for the public API, reporting p50/p99/p999
// latency (nanoseconds) as custom counters alongside throughput. This is the
// Google Benchmark harness referenced by the benchmarking resume bullet; the
// full head-to-head vs RocksDB lives in bench_vs_rocksdb.cpp.
//
// Run: ./bench_gbench   (add --benchmark_min_time=1x etc. as desired)

#include <algorithm>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "lsmdb/db.h"

namespace {

constexpr int kN = 100000;

std::string KeyOf(int n) {
  char b[16];
  snprintf(b, sizeof(b), "key%010d", n);
  return std::string(b);
}
std::string ValueOf(int n) {
  std::string s = std::to_string(n) + "#";
  s.resize(100, 'v');
  return s;
}

// A single shared, pre-populated database for the read benchmarks.
lsmdb::DB* SharedDB() {
  static lsmdb::DB* db = [] {
    if (system("rm -rf /tmp/lsmdb_gbench_read") != 0) { /* best effort */ }
    lsmdb::Options o;
    o.sync = false;
    std::unique_ptr<lsmdb::DB> d;
    lsmdb::DB::Open(o, "/tmp/lsmdb_gbench_read", &d);
    for (int i = 0; i < kN; ++i) d->Put(KeyOf(i), ValueOf(i));
    d->TEST_FlushMemTable();
    d->TEST_WaitForCompactions();
    return d.release();
  }();
  return db;
}

void ReportPercentiles(benchmark::State& state, std::vector<double>& lat) {
  if (lat.empty()) return;
  std::sort(lat.begin(), lat.end());
  auto pct = [&](double q) {
    size_t i = static_cast<size_t>(q * lat.size());
    return lat[std::min(i, lat.size() - 1)];
  };
  state.counters["p50_ns"] = pct(0.50);
  state.counters["p99_ns"] = pct(0.99);
  state.counters["p999_ns"] = pct(0.999);
}

}  // namespace

static void BM_RandomRead(benchmark::State& state) {
  lsmdb::DB* db = SharedDB();
  std::mt19937 rng(99);
  std::uniform_int_distribution<int> pick(0, kN - 1);
  std::vector<double> lat;
  std::string v;
  for (auto _ : state) {
    std::string k = KeyOf(pick(rng));
    auto t0 = std::chrono::steady_clock::now();
    db->Get(k, &v);
    auto ns = std::chrono::duration<double, std::nano>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    lat.push_back(ns);
  }
  state.SetItemsProcessed(state.iterations());
  ReportPercentiles(state, lat);
}
BENCHMARK(BM_RandomRead);

static void BM_SeqWrite(benchmark::State& state) {
  if (system("rm -rf /tmp/lsmdb_gbench_write") != 0) { /* best effort */ }
  lsmdb::Options o;
  o.sync = false;
  std::unique_ptr<lsmdb::DB> db;
  lsmdb::DB::Open(o, "/tmp/lsmdb_gbench_write", &db);
  std::vector<double> lat;
  int i = 0;
  for (auto _ : state) {
    auto t0 = std::chrono::steady_clock::now();
    db->Put(KeyOf(i), ValueOf(i));
    auto ns = std::chrono::duration<double, std::nano>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    lat.push_back(ns);
    ++i;
  }
  state.SetItemsProcessed(state.iterations());
  ReportPercentiles(state, lat);
}
BENCHMARK(BM_SeqWrite);

BENCHMARK_MAIN();
