// Phase 7 headline benchmark: lsmdb vs RocksDB across five workloads.
//
// Both engines are configured comparably (no compression, ~4 MB memtable, async
// WAL) and driven through the same key/value sizes and access patterns. For
// each workload we report throughput (ops/sec) and per-operation latency
// percentiles (p50/p99/p999); write amplification is reported from each engine's
// own accounting. The printed table is committed as the evidence behind the
// top-line resume bullet.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "lsmdb/db.h"

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/statistics.h>

using Clock = std::chrono::steady_clock;

static const int kN = 200000;
static const int kValueSize = 100;

static std::string KeyOf(int n) {
  char b[16];
  snprintf(b, sizeof(b), "key%010d", n);
  return std::string(b);
}
static std::string MakeValue(int n) {
  std::string s = std::to_string(n) + "#";
  s.resize(kValueSize, 'v');
  return s;
}

struct Metrics {
  double ops_per_sec = 0;
  double p50 = 0, p99 = 0, p999 = 0;  // nanoseconds
};

static Metrics Summarize(std::vector<double>& lat_ns, double elapsed_s) {
  Metrics m;
  std::sort(lat_ns.begin(), lat_ns.end());
  auto pct = [&](double q) {
    size_t i = static_cast<size_t>(q * lat_ns.size());
    if (i >= lat_ns.size()) i = lat_ns.size() - 1;
    return lat_ns[i];
  };
  m.p50 = pct(0.50);
  m.p99 = pct(0.99);
  m.p999 = pct(0.999);
  m.ops_per_sec = lat_ns.size() / elapsed_s;
  return m;
}

// Runs the 5 workloads given put/get closures; returns metrics per workload.
template <typename PutFn, typename GetFn, typename FlushFn>
struct Runner {
  PutFn put;
  GetFn get;
  FlushFn flush;

  Metrics SeqWrite() {
    std::vector<double> lat;
    lat.reserve(kN);
    auto t0 = Clock::now();
    for (int i = 0; i < kN; ++i) {
      auto a = Clock::now();
      put(KeyOf(i), MakeValue(i));
      lat.push_back(std::chrono::duration<double, std::nano>(Clock::now() - a).count());
    }
    double el = std::chrono::duration<double>(Clock::now() - t0).count();
    return Summarize(lat, el);
  }

  Metrics RandomWrite(const std::vector<int>& order) {
    std::vector<double> lat;
    lat.reserve(order.size());
    auto t0 = Clock::now();
    for (int idx : order) {
      auto a = Clock::now();
      put(KeyOf(idx), MakeValue(idx));
      lat.push_back(std::chrono::duration<double, std::nano>(Clock::now() - a).count());
    }
    double el = std::chrono::duration<double>(Clock::now() - t0).count();
    return Summarize(lat, el);
  }

  Metrics Reads(const std::vector<int>& order) {
    std::vector<double> lat;
    lat.reserve(order.size());
    std::string v;
    auto t0 = Clock::now();
    for (int idx : order) {
      auto a = Clock::now();
      get(KeyOf(idx), &v);
      lat.push_back(std::chrono::duration<double, std::nano>(Clock::now() - a).count());
    }
    double el = std::chrono::duration<double>(Clock::now() - t0).count();
    return Summarize(lat, el);
  }

  Metrics Mixed(const std::vector<int>& order) {
    std::vector<double> lat;
    lat.reserve(order.size());
    std::string v;
    std::mt19937 rng(7);
    auto t0 = Clock::now();
    for (size_t i = 0; i < order.size(); ++i) {
      int idx = order[i];
      auto a = Clock::now();
      if (rng() & 1) {
        put(KeyOf(idx), MakeValue(idx));
      } else {
        get(KeyOf(idx), &v);
      }
      lat.push_back(std::chrono::duration<double, std::nano>(Clock::now() - a).count());
    }
    double el = std::chrono::duration<double>(Clock::now() - t0).count();
    return Summarize(lat, el);
  }
};

static void PrintRow(const char* engine, const char* wl, const Metrics& m) {
  printf("%-8s %-13s %12.0f  %10.0f %10.0f %10.0f\n", engine, wl,
         m.ops_per_sec, m.p50, m.p99, m.p999);
}

int main() {
  std::vector<int> rnd(kN);
  for (int i = 0; i < kN; ++i) rnd[i] = i;
  std::mt19937 rng(12345);
  std::shuffle(rnd.begin(), rnd.end(), rng);

  printf("=== lsmdb vs RocksDB  (N=%d keys, %d-byte values, no compression, "
         "async WAL) ===\n\n", kN, kValueSize);
  printf("%-8s %-13s %12s  %10s %10s %10s\n", "engine", "workload", "ops/sec",
         "p50(ns)", "p99(ns)", "p999(ns)");
  printf("--------------------------------------------------------------------------------\n");

  double lsm_wamp = 0, rocks_wamp = 0;

  // ---------------- lsmdb ----------------
  {
    if (system("rm -rf /tmp/lsmdb_vs_a /tmp/lsmdb_vs_b") != 0) { }
    lsmdb::Options o;
    o.sync = false;
    o.memtable_flush_threshold = 4 * 1024 * 1024;
    o.l1_target_bytes = 8 * 1024 * 1024;
    o.target_file_size = 2 * 1024 * 1024;

    // Populate (random writes) into DB "a" and read from it.
    std::unique_ptr<lsmdb::DB> db;
    lsmdb::DB::Open(o, "/tmp/lsmdb_vs_a", &db);
    Runner<std::function<void(const std::string&, const std::string&)>,
           std::function<void(const std::string&, std::string*)>,
           std::function<void()>>
        r{[&](const std::string& k, const std::string& v) { db->Put(k, v); },
          [&](const std::string& k, std::string* v) { db->Get(k, v); },
          [&]() {}};

    Metrics rw = r.RandomWrite(rnd);
    db->TEST_FlushMemTable();
    db->TEST_WaitForCompactions();
    Metrics rr = r.Reads(rnd);
    std::vector<int> seq(kN);
    for (int i = 0; i < kN; ++i) seq[i] = i;
    Metrics sr = r.Reads(seq);
    Metrics mx = r.Mixed(rnd);
    lsm_wamp = db->GetStats().write_amplification;

    // Sequential write into a clean DB "b".
    std::unique_ptr<lsmdb::DB> db2;
    lsmdb::DB::Open(o, "/tmp/lsmdb_vs_b", &db2);
    Runner<std::function<void(const std::string&, const std::string&)>,
           std::function<void(const std::string&, std::string*)>,
           std::function<void()>>
        r2{[&](const std::string& k, const std::string& v) { db2->Put(k, v); },
           [&](const std::string& k, std::string* v) { db2->Get(k, v); },
           [&]() {}};
    Metrics sw = r2.SeqWrite();

    PrintRow("lsmdb", "seq-write", sw);
    PrintRow("lsmdb", "random-write", rw);
    PrintRow("lsmdb", "seq-read", sr);
    PrintRow("lsmdb", "random-read", rr);
    PrintRow("lsmdb", "mixed-50/50", mx);
  }

  // ---------------- RocksDB ----------------
  {
    if (system("rm -rf /tmp/rocks_vs_a /tmp/rocks_vs_b") != 0) { }
    rocksdb::Options o;
    o.create_if_missing = true;
    o.compression = rocksdb::kNoCompression;
    o.write_buffer_size = 4 * 1024 * 1024;
    o.max_bytes_for_level_base = 8 * 1024 * 1024;
    o.target_file_size_base = 2 * 1024 * 1024;
    o.statistics = rocksdb::CreateDBStatistics();
    rocksdb::WriteOptions wo;  // sync=false
    rocksdb::ReadOptions ro;

    rocksdb::DB* db = nullptr;
    rocksdb::DB::Open(o, "/tmp/rocks_vs_a", &db);
    Runner<std::function<void(const std::string&, const std::string&)>,
           std::function<void(const std::string&, std::string*)>,
           std::function<void()>>
        r{[&](const std::string& k, const std::string& v) { db->Put(wo, k, v); },
          [&](const std::string& k, std::string* v) { db->Get(ro, k, v); },
          [&]() {}};

    Metrics rw = r.RandomWrite(rnd);
    rocksdb::FlushOptions fo;
    db->Flush(fo);
    Metrics rr = r.Reads(rnd);
    std::vector<int> seq(kN);
    for (int i = 0; i < kN; ++i) seq[i] = i;
    Metrics sr = r.Reads(seq);
    Metrics mx = r.Mixed(rnd);

    uint64_t flush_b =
        o.statistics->getTickerCount(rocksdb::FLUSH_WRITE_BYTES);
    uint64_t compact_b =
        o.statistics->getTickerCount(rocksdb::COMPACT_WRITE_BYTES);
    uint64_t user_b = (uint64_t)kN * (13 + kValueSize);  // 13-byte key + value
    rocks_wamp = user_b ? double(flush_b + compact_b) / user_b : 0;
    delete db;

    rocksdb::Options o2 = o;
    o2.statistics = rocksdb::CreateDBStatistics();
    rocksdb::DB* db2 = nullptr;
    rocksdb::DB::Open(o2, "/tmp/rocks_vs_b", &db2);
    Runner<std::function<void(const std::string&, const std::string&)>,
           std::function<void(const std::string&, std::string*)>,
           std::function<void()>>
        r2{[&](const std::string& k, const std::string& v) { db2->Put(wo, k, v); },
           [&](const std::string& k, std::string* v) { db2->Get(ro, k, v); },
           [&]() {}};
    Metrics sw = r2.SeqWrite();
    delete db2;

    PrintRow("rocksdb", "seq-write", sw);
    PrintRow("rocksdb", "random-write", rw);
    PrintRow("rocksdb", "seq-read", sr);
    PrintRow("rocksdb", "random-read", rr);
    PrintRow("rocksdb", "mixed-50/50", mx);
  }

  printf("\nwrite amplification: lsmdb %.2fx   rocksdb %.2fx\n", lsm_wamp,
         rocks_wamp);
  if (system("rm -rf /tmp/lsmdb_vs_a /tmp/lsmdb_vs_b /tmp/rocks_vs_a /tmp/rocks_vs_b") != 0) { }
  return 0;
}
