// Phase 5 measurement driver: leveled compaction and write amplification.
//
// Writes a large keyset (with overwrites and deletes) through the engine at
// small level thresholds so the data cascades through several levels, driven by
// the background compaction thread. Then reports the level layout and the write
// amplification = total SSTable bytes written (flush + compaction) / bytes of
// user data. Committed as the evidence behind the compaction bullet.

#include <cstdio>
#include <memory>
#include <string>

#include "lsmdb/db.h"

using lsmdb::DB;
using lsmdb::Options;
using lsmdb::Status;

static std::string KeyOf(int n) {
  char b[24];
  snprintf(b, sizeof(b), "key%09d", n);
  return std::string(b);
}
static std::string ValueOf(int n) {
  std::string s = std::to_string(n) + "-";
  s.resize(200, 'x');  // ~200-byte values
  return s;
}

int main() {
  const std::string dir = "/tmp/lsmdb_bench_compaction";
  // Best-effort clean slate.
  if (system(("rm -rf " + dir).c_str()) != 0) { /* best-effort cleanup */ }

  Options o;
  o.sync = false;  // measuring compaction write-amp, not WAL durability
  o.memtable_flush_threshold = 1 * 1024 * 1024;  // 1 MB memtable
  o.l0_compaction_trigger = 4;
  o.l1_target_bytes = 4 * 1024 * 1024;           // 4 MB base level
  o.level_size_multiplier = 10;
  o.target_file_size = 2 * 1024 * 1024;
  o.max_levels = 7;

  std::unique_ptr<DB> db;
  Status s = DB::Open(o, dir, &db);
  if (!s.ok()) { fprintf(stderr, "open: %s\n", s.ToString().c_str()); return 1; }

  const int N = 300000;
  // Round 1: initial load.
  for (int i = 0; i < N; ++i) db->Put(KeyOf(i), ValueOf(i));
  // Round 2: overwrite half the keys (drives compaction to collapse versions).
  for (int i = 0; i < N; i += 2) db->Put(KeyOf(i), ValueOf(i));
  // Round 3: delete a tenth.
  for (int i = 0; i < N; i += 10) db->Delete(KeyOf(i));

  db->TEST_FlushMemTable();
  db->TEST_WaitForCompactions();

  DB::Stats st = db->GetStats();

  printf("==== Phase 5: leveled compaction + write amplification ====\n");
  printf("operations         : %d puts + %d overwrites + %d deletes\n", N,
         N / 2, N / 10);
  printf("user data written  : %.2f MB\n", st.user_bytes / 1e6);
  printf("flush bytes        : %.2f MB\n", st.flush_bytes / 1e6);
  printf("compaction bytes   : %.2f MB\n", st.compaction_bytes / 1e6);
  printf("total SSTable bytes: %.2f MB\n", st.sstable_bytes_written / 1e6);
  printf("\n");
  printf("WRITE AMPLIFICATION: %.2fx  (SSTable bytes / user bytes)\n",
         st.write_amplification);
  printf("\n");
  int populated = 0;
  for (size_t l = 0; l < st.files_per_level.size(); ++l) {
    if (st.files_per_level[l] > 0) {
      ++populated;
      printf("  L%zu: %3d files  %8.2f MB\n", l, st.files_per_level[l],
             st.bytes_per_level[l] / 1e6);
    }
  }
  printf("\npopulated levels   : %d\n", populated);

  if (system(("rm -rf " + dir).c_str()) != 0) { /* best-effort cleanup */ }
  return 0;
}
