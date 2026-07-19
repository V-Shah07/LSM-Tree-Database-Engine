#include "lsmdb/db.h"

#include <sys/stat.h>

#include <cerrno>
#include <cstring>

namespace lsmdb {

DB::DB(const Options& options, std::string dir)
    : options_(options), dir_(std::move(dir)), mem_(new SkipList()) {
  wal_path_ = dir_ + "/wal.log";
}

DB::~DB() { wal_.Close(); }

Status DB::Open(const std::string& dir, std::unique_ptr<DB>* out) {
  return Open(Options(), dir, out);
}

Status DB::Open(const Options& options, const std::string& dir,
                std::unique_ptr<DB>* out) {
  if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
    return Status::IOError("mkdir " + dir + ": " + strerror(errno));
  }

  auto db = std::unique_ptr<DB>(new DB(options, dir));

  // Rebuild the memtable by replaying the WAL. Records are applied in log
  // order, so a later overwrite/tombstone for a key naturally wins.
  size_t replayed = 0;
  SkipList* mem = db->mem_.get();
  Status s = WalReplay(
      db->wal_path_,
      [mem](ValueType type, const std::string& key, const std::string& value) {
        if (type == ValueType::kTombstone) {
          mem->Delete(key);
        } else {
          mem->Put(key, value);
        }
      },
      &replayed);
  if (!s.ok()) return s;

  // Continue appending to the same log for subsequent writes.
  s = db->wal_.Open(db->wal_path_);
  if (!s.ok()) return s;

  *out = std::move(db);
  return Status::OK();
}

Status DB::Put(const std::string& key, const std::string& value) {
  // WAL first, then memtable: the write is durable before it becomes visible.
  Status s = wal_.AddRecord(ValueType::kValue, key, value, options_.sync);
  if (!s.ok()) return s;
  mem_->Put(key, value);
  return Status::OK();
}

Status DB::Delete(const std::string& key) {
  Status s = wal_.AddRecord(ValueType::kTombstone, key, std::string(),
                            options_.sync);
  if (!s.ok()) return s;
  mem_->Delete(key);
  return Status::OK();
}

LookupResult DB::Get(const std::string& key, std::string* value) const {
  return mem_->Get(key, value);
}

}  // namespace lsmdb
