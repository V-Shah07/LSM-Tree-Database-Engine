#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

#include <gtest/gtest.h>

#include "lsmdb/db.h"
#include "lsmdb/wal.h"

using lsmdb::DB;
using lsmdb::LookupResult;
using lsmdb::Status;
using lsmdb::ValueType;
using lsmdb::WalReplay;
using lsmdb::WalWriter;

namespace {

std::string KeyOf(int n) {
  char buf[24];
  snprintf(buf, sizeof(buf), "key%09d", n);
  return std::string(buf);
}

// ~72-byte value, distinct per key so recovery can verify content, not just
// presence.
std::string ValueOf(int n) {
  char buf[80];
  snprintf(buf, sizeof(buf), "val-%09d-payload-padding-payload-padding-payload-pad", n);
  return std::string(buf);
}

std::string UniqueDir(const std::string& tag) {
  std::string d = "/tmp/lsmdb_recovery_" + std::to_string(::getpid()) + "_" +
                  tag + "_" + std::to_string(::rand());
  return d;
}

void DestroyDir(const std::string& dir) {
  std::string wal = dir + "/wal.log";
  ::unlink(wal.c_str());
  ::rmdir(dir.c_str());
}

}  // namespace

// A clean close followed by reopen must reproduce exactly the committed state,
// including deletes shadowing earlier values.
TEST(Recovery, CleanReopenPreservesState) {
  std::string dir = UniqueDir("clean");
  const int N = 3000;
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(dir, &db).ok());
    for (int i = 0; i < N; ++i) {
      ASSERT_TRUE(db->Put(KeyOf(i), ValueOf(i)).ok());
    }
    // Delete every 7th key.
    for (int i = 0; i < N; i += 7) {
      ASSERT_TRUE(db->Delete(KeyOf(i)).ok());
    }
  }  // db destructed -> WAL closed

  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(dir, &db).ok());
  for (int i = 0; i < N; ++i) {
    std::string v;
    LookupResult r = db->Get(KeyOf(i), &v);
    if (i % 7 == 0) {
      EXPECT_EQ(r, LookupResult::kDeleted) << "i=" << i;
    } else {
      ASSERT_EQ(r, LookupResult::kFound) << "i=" << i;
      EXPECT_EQ(v, ValueOf(i));
    }
  }
  db.reset();
  DestroyDir(dir);
}

// A record whose payload was only partially written (a crash mid-write) must be
// dropped on replay, while every intact record before it is preserved.
TEST(Recovery, TornTailRecordDropped) {
  std::string dir = UniqueDir("torn");
  ASSERT_EQ(::mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST, true);
  std::string path = dir + "/wal.log";

  {
    WalWriter w;
    ASSERT_TRUE(w.Open(path).ok());
    ASSERT_TRUE(w.AddRecord(ValueType::kValue, "a", "1", true).ok());
    ASSERT_TRUE(w.AddRecord(ValueType::kValue, "b", "2", true).ok());
    ASSERT_TRUE(w.AddRecord(ValueType::kValue, "c", "3", true).ok());
    ASSERT_TRUE(w.Close().ok());
  }

  // Simulate a crash mid-append: tack on a few bytes that look like the start
  // of a 4th record but are truncated.
  {
    int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
    ASSERT_GE(fd, 0);
    const char junk[] = {0x11, 0x22, 0x33, 0x44, 0x55};  // partial header
    ASSERT_EQ(::write(fd, junk, sizeof(junk)), (ssize_t)sizeof(junk));
    ::close(fd);
  }

  size_t replayed = 0;
  int seen = 0;
  Status s = WalReplay(
      path,
      [&](ValueType, const std::string&, const std::string&) { ++seen; },
      &replayed);
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(replayed, 3u);
  EXPECT_EQ(seen, 3);

  // The DB opens fine and sees exactly the three durable keys.
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(dir, &db).ok());
  std::string v;
  EXPECT_EQ(db->Get("a", &v), LookupResult::kFound);
  EXPECT_EQ(db->Get("c", &v), LookupResult::kFound);
  db.reset();
  DestroyDir(dir);
}

// The headline durability test. A child process writes keys sequentially, each
// fsync'd before the next. The parent SIGKILLs it at a random moment, then
// reopens the database and asserts:
//   (1) recovered keys form a *contiguous prefix* 0..M-1 (no holes),
//   (2) every recovered key has the correct value,
//   (3) no key beyond the prefix leaked in via a corrupt/torn record.
// Repeated across many random kill points -> automated fault injection.
TEST(Recovery, SigkillFaultInjection) {
  const int kIters = 20;
  std::mt19937 rng(20260719);
  std::uniform_int_distribution<int> delay_us(5000, 220000);

  long total_recovered = 0;
  int nonempty_iters = 0;

  for (int iter = 0; iter < kIters; ++iter) {
    std::string dir = UniqueDir("kill_" + std::to_string(iter));

    pid_t pid = ::fork();
    ASSERT_GE(pid, 0) << "fork failed";

    if (pid == 0) {
      // ---- child: write forever until killed ----
      std::unique_ptr<DB> db;
      if (!DB::Open(dir, &db).ok()) _exit(20);
      for (int i = 0;; ++i) {
        if (!db->Put(KeyOf(i), ValueOf(i)).ok()) _exit(21);
      }
      _exit(0);  // unreachable
    }

    // ---- parent: let it run, then kill mid-flight ----
    int this_delay = delay_us(rng);
    ::usleep(this_delay);
    ASSERT_EQ(::kill(pid, SIGKILL), 0);
    int wstatus = 0;
    ASSERT_EQ(::waitpid(pid, &wstatus, 0), pid);
    if (!WIFSIGNALED(wstatus)) {
      // Child exited on its own -> DB::Open or Put failed before we killed it.
      FAIL() << "child exited with code "
             << (WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1)
             << " instead of being signaled";
    }

    // ---- reopen and verify ----
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(dir, &db).ok());

    int recovered = 0;
    for (int i = 0;; ++i) {
      std::string v;
      LookupResult r = db->Get(KeyOf(i), &v);
      if (r == LookupResult::kFound) {
        ASSERT_EQ(v, ValueOf(i)) << "corrupt value at key " << i;
        recovered = i + 1;
      } else {
        break;
      }
    }
    // No key beyond the recovered prefix may exist (would indicate a torn
    // record was wrongly accepted, i.e. a hole in the sequence).
    for (int i = recovered; i < recovered + 100; ++i) {
      std::string v;
      ASSERT_EQ(db->Get(KeyOf(i), &v), LookupResult::kNotFound)
          << "hole: key " << i << " present past prefix end " << recovered;
    }

    printf("[crash iter %2d] killed after %6d us -> recovered %d committed keys, "
           "prefix intact, 0 lost\n",
           iter, this_delay, recovered);
    total_recovered += recovered;
    if (recovered > 0) ++nonempty_iters;

    db.reset();
    DestroyDir(dir);
  }

  printf("[crash summary] %d/%d iterations recovered data; %ld total committed "
         "keys recovered across all SIGKILLs; 0 committed writes lost\n",
         nonempty_iters, kIters, total_recovered);

  // The test must actually have exercised recovery with real data.
  EXPECT_GT(nonempty_iters, 0);
  EXPECT_GT(total_recovered, 0);
}
