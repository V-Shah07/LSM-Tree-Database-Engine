# lsmdb — an LSM-Tree key-value storage engine in C++

A persistent key-value storage engine built from scratch in C++17, implementing a
**Log-Structured Merge tree** — the core structure underneath RocksDB, LevelDB,
and Cassandra. This is the storage layer that sits *under* a database: no SQL, no
query planner, just the engine that turns `Put`/`Get`/`Delete`/`Scan` into fast,
durable disk I/O.

Every component is hand-written — skip-list memtable, binary SSTable format,
write-ahead log, bloom filter, MurmurHash3, CRC32, leveled compaction, and an LRU
block cache — and every performance claim below is backed by a committed
benchmark or test log under [`evidence/`](evidence/).

## Architecture

```
          Put/Delete                         Get / Scan
              │                                   │
              ▼                                   ▼
      ┌───────────────┐   fsync           ┌───────────────────────────┐
      │  Write-Ahead  │◀──────────────────│   memtable (skip list)    │  newest
      │      Log      │                   └───────────────────────────┘
      └───────────────┘                          │ flush at threshold
                                                  ▼
                              ┌───────────────────────────────────────┐
                        L0    │  SSTable  SSTable  SSTable   (overlap) │
                              ├───────────────────────────────────────┤
                        L1    │  SSTable   SSTable        (sorted run) │  background
                              ├───────────────────────────────────────┤   compaction
                        L2    │  SSTable  SSTable  SSTable ...         │   thread
                              └───────────────────────────────────────┘  oldest
   each SSTable: [data blocks + CRC32] [bloom filter] [sparse index] [footer]
   shared across reads: LRU block cache
```

Writes hit the WAL (durable) then the in-memory skip list. When the memtable
fills it is flushed to an immutable, sorted **SSTable** at level 0. A background
thread **compacts** levels into the next one when they fill, dropping tombstones
and superseded versions. Reads check the memtable, then L0 (newest first), then
each deeper (non-overlapping) level — with a **bloom filter** skipping most
SSTables without touching disk and an **LRU block cache** absorbing hot reads.

## Design decisions (the interesting part)

- **Why an LSM tree over a B-tree?** Writes are batched in memory and flushed as
  large sequential runs, turning random writes into sequential I/O. A B-tree
  pays random-seek + page-split costs on every write. The LSM trade is read and
  space amplification, which bloom filters, a block cache, and compaction repay.
- **Skip list for the memtable** (not `std::map`): O(log n) ordered inserts/lookups
  like a balanced BST, but far simpler to get right — and it's what LevelDB uses.
- **Custom SSTable format**: CRC32-checksummed data blocks, a **sparse index**
  (one key per block), and a footer. Point lookups binary-search the index and
  scan a single block — O(log n), no full-file scan. Reads are `mmap`-based.
- **WAL + fsync before ack**: durability is the point. A write is not acknowledged
  until its record is `fsync`'d to the log; recovery replays the log and tolerates
  a torn tail record (crash mid-write) via its length prefix + CRC.
- **Bloom filter with tunable FP rate**: `k = (bits/key)·ln2` probes via double
  hashing over a from-scratch MurmurHash3. The single knob (bits/key) trades
  memory for fewer disk reads on missing keys.
- **Leveled compaction**: keeps each level ≥1 a non-overlapping sorted run by
  merging the chosen input with exactly the overlapping next-level files, which
  bounds and makes **write amplification** measurable. More aggressive compaction
  → faster reads (fewer SSTables) but higher write-amp; this is the central LSM
  tuning dial.
- **LRU block cache**: a cache hit skips the block copy out of the mmap *and* the
  CRC recomputation — the bulk of per-lookup CPU. Compaction/iteration bypass the
  cache so scans don't evict the hot set.

## Benchmarks

### vs RocksDB — `evidence/phase7_bench_vs_rocksdb.log`

Same machine, same workload: 200,000 keys, 100-byte values, no compression, async
WAL on both engines. lsmdb is configured comparably to RocksDB (~4 MB memtable,
8 MB base level, 2 MB files).

| engine  | workload      |     ops/sec | p50 (ns) | p99 (ns) | p999 (ns) |
|---------|---------------|------------:|---------:|---------:|----------:|
| lsmdb   | seq-write     |     223,567 |    1,051 |    4,058 |    23,958 |
| lsmdb   | random-write  | **308,131** |    1,428 |    6,001 |    25,686 |
| lsmdb   | seq-read      |   1,263,866 |      422 |   11,508 |    16,504 |
| lsmdb   | random-read   |     155,891 |    1,719 |   25,512 |    46,354 |
| lsmdb   | mixed-50/50   |     118,729 |    2,456 |   25,390 |    48,004 |
| rocksdb | seq-write     |     418,187 |    2,012 |    8,357 |    31,109 |
| rocksdb | random-write  |     251,267 |    3,492 |   11,903 |    37,251 |
| rocksdb | seq-read      |     250,518 |    3,746 |    9,509 |    27,502 |
| rocksdb | random-read   |     174,305 |    5,298 |   18,002 |    44,930 |
| rocksdb | mixed-50/50   |     149,191 |    5,439 |   21,776 |    49,926 |

**Write amplification: lsmdb 3.46× vs RocksDB 3.77×.**

Takeaways (honest): lsmdb **beats RocksDB on random-write throughput (1.2×)** and
has lower p50 latency across the board on this single-threaded, uncompressed
workload; it is **within ~10% of RocksDB on random reads** (156K vs 174K ops/sec,
25 µs vs 18 µs p99) and trails on sequential-write and mixed throughput. RocksDB
is a mature, multithreaded engine with far more features — landing in the same
ballpark from a from-scratch engine is the result worth reporting.

Percentiles are also produced under the **Google Benchmark** framework
(`bench_gbench`, `evidence/phase7_gbench.log`) as `p50_ns`/`p99_ns`/`p999_ns`
counters.

### Component benchmarks

- **Bloom filter** (`evidence/phase4_bloom_bench.log`): on missing-key lookups,
  **99.2% of disk block reads avoided** (1,000,000 → 8,020), ~51× faster; measured
  FP 0.80% vs 0.82% theoretical at 10 bits/key.
- **Block cache** (`evidence/phase6_cache_bench.log`): skewed workload,
  **97.2% hit rate**, mean read latency **11,239 ns → 810 ns (13.9×)**.
- **Compaction** (`evidence/phase5_compaction_bench.log`): write amplification
  **4.39× across 4 levels** with a background merge thread.
- **Crash recovery** (`evidence/phase3_crash_recovery.log`): 20 randomized
  `SIGKILL` injections, **0 committed writes lost**.

## Building

Requires a C++17 compiler, CMake ≥ 3.16, GoogleTest, and (optionally) RocksDB and
Google Benchmark for the comparison benchmarks.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure     # run the test suite

./build/bench_bloom          # bloom filter
./build/bench_compaction     # write amplification
./build/bench_cache          # block cache
./build/bench_vs_rocksdb     # head-to-head vs RocksDB (needs librocksdb-dev)
./build/bench_gbench         # Google Benchmark microbenchmarks
```

Or with Docker:

```bash
docker build -t lsmdb .
docker run --rm lsmdb            # builds + runs the full test suite
```

## Public API

```cpp
lsmdb::Options options;
std::unique_ptr<lsmdb::DB> db;
lsmdb::DB::Open(options, "/path/to/dir", &db);

db->Put("key", "value");
db->Delete("key");                        // tombstone

std::string value;
lsmdb::LookupResult r = db->Get("key", &value);   // kFound / kDeleted / kNotFound

std::vector<std::pair<std::string, std::string>> rows;
db->Scan("a", "m", &rows);                // live rows in [a, m), merged & sorted
```

## Repository layout

```
include/lsmdb/   public headers (skiplist, sstable, wal, bloom, cache, db, ...)
src/             implementations
tests/           GoogleTest suites (39 cases: skiplist, sstable, crc32, bloom,
                 recovery/SIGKILL, compaction, cache, scan)
bench/           measurement drivers + the RocksDB comparison
evidence/        committed test & benchmark logs backing every claim above
```
