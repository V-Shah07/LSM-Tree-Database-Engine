# LSM-Tree Key-Value Storage Engine — Build Spec

> **For the autonomous coding session.** Single source of truth for this repo. Build in phase order. Every phase ends with **PROVE IT** — a passing test or a benchmark number. The resume bullets at the bottom are the contract: every one must be literally true and backed by committed test output or a benchmark log.

---

## 0. What you're building & what actually matters

A persistent key-value storage engine in **C++ (C++17)** implementing a Log-Structured Merge Tree — the core structure under RocksDB / LevelDB / Cassandra. You're building the storage layer that sits *under* a database: no SQL parser, no query planner, just the engine that reads/writes to disk fast. Every design decision is yours (page format, compaction, bloom sizing, WAL format).

**Unlike the other two projects, this one barely compresses** — it's algorithmic, and nearly every component directly backs a bullet. Don't cut components; simplify *within* them. The single most credibility-defining artifact is the **benchmark vs RocksDB/LevelDB with real p50/p99 latency and write-amplification numbers.** Prioritize getting to a runnable benchmark early enough that the numbers are real.

### Scope decisions already made (do not re-expand)
- **Language:** C++17. One choice, stay consistent. CMake build. (Matches your existing C++ backtester — reuse muscle memory.)
- **Memtable structure:** implement a **skip list from scratch** (what LevelDB uses; simpler to get right than a red-black tree). No `std::map` for the memtable — that's the "from scratch" bullet.
- **Compaction:** leveled compaction, but a **simplified "leveled-lite"** is acceptable (see Phase 5) as long as write-amplification is measured and levels are real.
- **Benchmark baseline:** RocksDB or LevelDB via its C++ API. Getting this linked and comparable is a priority, not a stretch goal.
- **Priority if time runs out:** a working engine with a real RocksDB benchmark beats an extra feature. Bloom filter and cache are higher-value-per-hour than exotic compaction. If compaction is eating the clock, ship simplified leveled-lite with measured write-amp and move to benchmarking.

---

## 1. Tech stack (final)

| Layer | Choice |
|---|---|
| Language | C++17 |
| Memtable | custom skip list (no STL map) |
| Storage | custom binary SSTable format, mmap reads, CRC32 |
| Bloom filter | custom bit array + k hashes (MurmurHash or xxHash) |
| Cache | custom LRU block cache |
| Benchmark | Google Benchmark |
| Tests | GoogleTest |
| Baseline | RocksDB or LevelDB (link its API) |
| Build | CMake |
| CI | GitHub Actions |
| Container | Docker |

---

## 2. Build order (phases)

Each phase: build → **PROVE IT** (test passes / number logged) → commit. Keep CI green.

### Phase 1 — Memtable (skip list from scratch) ⭐
- Implement a skip list from scratch. `Put/Get/Delete` fully in memory, keys sorted.
- No STL map. Configurable size threshold for later flushing.

**PROVE IT:** unit tests for sorted order, insert, lookup, delete, and duplicate-key overwrite. All green in CI.

### Phase 2 — SSTable writer + reader ⭐
- Flush memtable to an immutable on-disk SSTable in your own format: data block section (sorted KV) + sparse index block (key → byte offset) + footer pointing to the index.
- Reader: binary-search the index, seek directly to the data block — no full scan.
- `mmap` reads. CRC32 checksums on blocks.

**PROVE IT:** flush a known keyset, read every key back, verify values; corrupt a byte and assert CRC catches it. Tests green. (Backs the "O(log n) point lookups without full file scans" bullet.)

### Phase 3 — Write-Ahead Log + crash recovery ⭐ (correctness milestone)
- Append every write to a WAL (sequential encoded KV ops + CRC32) **before** it hits the memtable. fsync before ack.
- On startup, replay WAL entries not yet flushed to SSTables.

**PROVE IT:** automated crash test — write N keys, `SIGKILL` at a random point, restart, assert every committed write survived. Commit the test output. This is the "zero data loss, verified via fault injection" bullet.

### Phase 4 — Bloom filter ⭐
- Custom bloom filter (bit array + k hash functions, MurmurHash/xxHash) attached to each SSTable.
- Check bloom before touching an SSTable on disk — O(1) "definitely not here."
- Tunable false-positive rate.

**PROVE IT:** verify measured false-positive rate ≈ theoretical for chosen bits/keys; benchmark missing-key lookups with vs without bloom and log the speedup / disk-read reduction %. That % is the bloom bullet.

### Phase 5 — Compaction engine ⭐ (hardest piece — simplify if needed)
- Merge sort over multiple sorted SSTable iterators; drop tombstones + superseded duplicate keys, keep latest version.
- Leveled compaction: levels ~10× each; compaction triggers when a level fills. **Leveled-lite is acceptable** — the requirements are (a) real multiple levels, (b) background merge thread, (c) **measured write amplification** before/after. Don't gold-plate the level-selection heuristics.

**PROVE IT:** write enough to trigger ≥2 levels of compaction; verify data integrity post-merge (all live keys still correct, deletes gone); log write-amplification (bytes written to disk / bytes of user data). That number is the compaction bullet.

### Phase 6 — Buffer pool / LRU block cache ⭐
- Custom LRU cache of frequently-accessed SSTable blocks. Configurable size. Hit/miss metrics.

**PROVE IT:** benchmark read latency with vs without cache under a skewed workload; log cache hit rate and the latency improvement.

### Phase 7 — Public API + benchmark vs RocksDB ⭐⭐ (the credibility bullet)
- Public API: `Put`, `Get`, `Delete` (tombstone), `Scan(start, end)` via a merge iterator across memtable + all SSTables.
- **Benchmark suite (Google Benchmark)** against RocksDB/LevelDB on: sequential write, random write, sequential read, random read, mixed. Report throughput (ops/sec), latency **p50/p99/p999**, and **write amplification**.
- README frames the design decisions as the contribution. GitHub Actions CI. Dockerfile.

**PROVE IT:** commit the full benchmark output table (your engine vs baseline across all workloads). These are the headline numbers for the top bullet — every X/Y/Z comes from this table.

---

## 3. Where to spend vs save time

- **Spend** on: skip list correctness, WAL crash recovery (the fsync/replay must be truly correct), and getting the RocksDB benchmark linked and running. These back the strongest, most-scrutinized bullets.
- **Save** on: exotic compaction heuristics (leveled-lite is fine), fancy configurability, and anything not measured. If it doesn't produce a number or pass a test, it isn't earning its place in a 4-day sprint.
- **Highest value-per-hour:** bloom filter and block cache — small code, big measurable wins, easy bullets.

---

## 4. Interview talking points (put in README)

- **Why LSM over B-tree?** Write-heavy workloads benefit from sequential I/O; random B-tree writes cause page splits + random seeks.
- **Compaction read/write tradeoff?** More aggressive compaction → faster reads (fewer SSTables) but higher write amplification.
- **Why bloom filters?** Avoid O(n) disk reads for missing keys; FP rate tunable to memory budget.
- **How does the WAL guarantee durability?** Every write fsync'd to WAL before ack; recovery replays uncommitted WAL entries on startup.
- **What is write amplification and why care?** Each user byte gets rewritten through compaction; lower write-amp → longer SSD life + higher sustained throughput.
- **Why a skip list for the memtable?** O(log n) like a balanced BST but simpler to implement correctly.

---

## 5. Resume bullets (the contract — every one must end up TRUE)

Fill X/Y/Z from the committed benchmark table and test logs. Never invent numbers.

- Built a persistent LSM-Tree key-value storage engine in C++ from scratch, achieving **X ops/sec write throughput and Y µs p99 read latency — within Z% of RocksDB** (from Phase 7 benchmark table).
- Implemented a custom SSTable format with sparse index, memory-mapped I/O, and CRC32 checksums enabling O(log n) point lookups without full-file scans.
- Designed a bloom filter with tunable false-positive rate reducing unnecessary disk reads by **X% on missing-key lookups** (Phase 4 log).
- Built a leveled compaction engine with a background merge thread maintaining **write amplification of Xx across Z levels** (Phase 5 log).
- Implemented a write-ahead log with crash recovery guaranteeing zero data loss for committed writes, **verified via automated SIGKILL fault-injection tests** (Phase 3).
- Benchmarked against RocksDB/LevelDB across sequential-write, random-read, and mixed workloads using Google Benchmark with p50/p99/p999 latency histograms.

> **Bullet honesty rule:** the "within Z% of RocksDB" claim must come from your own benchmark run on the same machine/workload. If your engine is 5× slower, say the true multiple — "within 5× of RocksDB on random reads" is still a strong, honest bullet for a from-scratch engine. Don't claim parity you didn't measure.
