#include "lsmdb/db.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <set>

#include "lsmdb/coding.h"
#include "lsmdb/merging_iterator.h"

namespace lsmdb {
namespace {

constexpr size_t kOutputBlockSize = 4096;

std::string NumberToName(uint64_t number) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%06llu.sst", static_cast<unsigned long long>(number));
  return std::string(buf);
}

bool ParseFileNumber(const std::string& name, uint64_t* number) {
  // Expect "<digits>.sst".
  size_t dot = name.find('.');
  if (dot == std::string::npos || name.substr(dot) != ".sst") return false;
  if (dot == 0) return false;
  uint64_t n = 0;
  for (size_t i = 0; i < dot; ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
    n = n * 10 + (name[i] - '0');
  }
  *number = n;
  return true;
}

// Does [f.smallest, f.largest] intersect [lo, hi]?
bool Overlaps(const std::string& fsmall, const std::string& flarge,
              const std::string& lo, const std::string& hi) {
  return flarge >= lo && fsmall <= hi;
}

}  // namespace

DB::DB(const Options& options, std::string dir)
    : options_(options), dir_(std::move(dir)), mem_(new SkipList()) {
  wal_path_ = dir_ + "/wal.log";
}

DB::~DB() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    shutting_down_ = true;
    bg_cv_.notify_all();
  }
  if (bg_thread_.joinable()) bg_thread_.join();
  wal_.Close();
}

std::string DB::FilePath(uint64_t number) const {
  return dir_ + "/" + NumberToName(number);
}
std::string DB::ManifestPath() const { return dir_ + "/MANIFEST"; }

Status DB::Open(const std::string& dir, std::unique_ptr<DB>* out) {
  return Open(Options(), dir, out);
}

Status DB::Open(const Options& options, const std::string& dir,
                std::unique_ptr<DB>* out) {
  if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
    return Status::IOError("mkdir " + dir + ": " + strerror(errno));
  }
  auto db = std::unique_ptr<DB>(new DB(options, dir));
  Status s = db->Recover();
  if (!s.ok()) return s;

  db->bg_thread_ = std::thread([raw = db.get()] { raw->BackgroundLoop(); });
  *out = std::move(db);
  return Status::OK();
}

Status DB::Recover() {
  Status s = LoadManifest();
  if (!s.ok()) return s;
  if (!current_) {
    current_ = std::make_shared<Version>();
    current_->levels.resize(1);  // L0
  }

  // Replay the WAL into the memtable. Anything already captured in an SSTable
  // is simply re-applied on top and deduplicated by read priority, so a crash
  // between flush and WAL truncation costs nothing but redundant work.
  SkipList* mem = mem_.get();
  size_t replayed = 0;
  s = WalReplay(
      wal_path_,
      [mem](ValueType type, const std::string& key, const std::string& value) {
        if (type == ValueType::kTombstone) {
          mem->Delete(key);
        } else {
          mem->Put(key, value);
        }
      },
      &replayed);
  if (!s.ok()) return s;

  s = wal_.Open(wal_path_);
  if (!s.ok()) return s;

  RemoveOrphanFiles(current_);
  return Status::OK();
}

Status DB::LoadManifest() {
  int fd = ::open(ManifestPath().c_str(), O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) return Status::OK();  // fresh database
    return Status::IOError("open manifest: " + std::string(strerror(errno)));
  }
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return Status::IOError("fstat manifest");
  }
  std::string buf;
  buf.resize(static_cast<size_t>(st.st_size));
  size_t off = 0;
  while (off < buf.size()) {
    ssize_t r = ::read(fd, &buf[off], buf.size() - off);
    if (r <= 0) break;
    off += static_cast<size_t>(r);
  }
  ::close(fd);
  buf.resize(off);

  const char* p = buf.data();
  size_t pos = 0;
  auto need = [&](size_t n) { return pos + n <= buf.size(); };
  if (!need(12)) return Status::Corruption("manifest too short");

  uint64_t next_file = DecodeFixed64(p + pos); pos += 8;
  uint32_t num_levels = DecodeFixed32(p + pos); pos += 4;

  auto v = std::make_shared<Version>();
  v->levels.resize(std::max<uint32_t>(num_levels, 1));
  uint64_t max_number = 0;

  for (uint32_t lvl = 0; lvl < num_levels; ++lvl) {
    if (!need(4)) return Status::Corruption("manifest truncated (level)");
    uint32_t num_files = DecodeFixed32(p + pos); pos += 4;
    for (uint32_t f = 0; f < num_files; ++f) {
      if (!need(24)) return Status::Corruption("manifest truncated (file)");
      auto fm = std::make_shared<FileMeta>();
      fm->number = DecodeFixed64(p + pos); pos += 8;
      fm->file_size = DecodeFixed64(p + pos); pos += 8;
      fm->num_entries = DecodeFixed64(p + pos); pos += 8;
      if (!need(4)) return Status::Corruption("manifest truncated (skey)");
      uint32_t sl = DecodeFixed32(p + pos); pos += 4;
      if (!need(sl)) return Status::Corruption("manifest truncated (skey2)");
      fm->smallest.assign(p + pos, sl); pos += sl;
      if (!need(4)) return Status::Corruption("manifest truncated (lkey)");
      uint32_t ll = DecodeFixed32(p + pos); pos += 4;
      if (!need(ll)) return Status::Corruption("manifest truncated (lkey2)");
      fm->largest.assign(p + pos, ll); pos += ll;

      std::unique_ptr<SSTableReader> reader;
      Status s = SSTableReader::Open(FilePath(fm->number), &reader);
      if (!s.ok()) return s;
      fm->reader = std::move(reader);
      max_number = std::max(max_number, fm->number);
      v->levels[lvl].push_back(std::move(fm));
    }
  }

  current_ = v;
  next_file_number_ = std::max(next_file, max_number + 1);
  return Status::OK();
}

Status DB::WriteManifest(const VersionPtr& v) {
  std::string buf;
  PutFixed64(&buf, next_file_number_);
  PutFixed32(&buf, static_cast<uint32_t>(v->levels.size()));
  for (const auto& level : v->levels) {
    PutFixed32(&buf, static_cast<uint32_t>(level.size()));
    for (const auto& f : level) {
      PutFixed64(&buf, f->number);
      PutFixed64(&buf, f->file_size);
      PutFixed64(&buf, f->num_entries);
      PutLengthPrefixed(&buf, f->smallest);
      PutLengthPrefixed(&buf, f->largest);
    }
  }

  std::string tmp = ManifestPath() + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return Status::IOError("open manifest.tmp");
  size_t off = 0;
  while (off < buf.size()) {
    ssize_t r = ::write(fd, buf.data() + off, buf.size() - off);
    if (r < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return Status::IOError("write manifest.tmp");
    }
    off += static_cast<size_t>(r);
  }
  if (::fsync(fd) != 0) { ::close(fd); return Status::IOError("fsync manifest"); }
  ::close(fd);
  // Atomic swap: a crash leaves either the old or the new manifest intact.
  if (::rename(tmp.c_str(), ManifestPath().c_str()) != 0) {
    return Status::IOError("rename manifest");
  }
  return Status::OK();
}

void DB::RemoveOrphanFiles(const VersionPtr& v) {
  std::set<uint64_t> live;
  for (const auto& level : v->levels) {
    for (const auto& f : level) live.insert(f->number);
  }
  DIR* d = ::opendir(dir_.c_str());
  if (!d) return;
  struct dirent* ent;
  while ((ent = ::readdir(d)) != nullptr) {
    uint64_t number;
    if (ParseFileNumber(ent->d_name, &number) && live.count(number) == 0) {
      ::unlink(FilePath(number).c_str());
    }
  }
  ::closedir(d);
}

// ---- writes -----------------------------------------------------------------

Status DB::Put(const std::string& key, const std::string& value) {
  Status s = wal_.AddRecord(ValueType::kValue, key, value, options_.sync);
  if (!s.ok()) return s;
  mem_->Put(key, value);
  user_bytes_ += key.size() + value.size();
  if (mem_->ApproximateMemoryUsage() >= options_.memtable_flush_threshold) {
    return FlushMemTable();
  }
  return Status::OK();
}

Status DB::Delete(const std::string& key) {
  Status s = wal_.AddRecord(ValueType::kTombstone, key, std::string(),
                            options_.sync);
  if (!s.ok()) return s;
  mem_->Delete(key);
  user_bytes_ += key.size();
  if (mem_->ApproximateMemoryUsage() >= options_.memtable_flush_threshold) {
    return FlushMemTable();
  }
  return Status::OK();
}

Status DB::TEST_FlushMemTable() { return FlushMemTable(); }

Status DB::FlushMemTable() {
  if (mem_->Size() == 0) return Status::OK();

  uint64_t number;
  {
    std::lock_guard<std::mutex> lk(mu_);
    number = next_file_number_++;
  }
  const std::string path = FilePath(number);

  SSTableWriter w(path, kOutputBlockSize, options_.bloom_bits_per_key);
  Status s = w.Open();
  if (!s.ok()) return s;
  auto it = mem_->NewIterator();
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    ValueType t = it.IsTombstone() ? ValueType::kTombstone : ValueType::kValue;
    s = w.Add(it.key(), it.value(), t);
    if (!s.ok()) return s;
  }
  s = w.Finish();
  if (!s.ok()) return s;

  std::unique_ptr<SSTableReader> reader;
  s = SSTableReader::Open(path, &reader);
  if (!s.ok()) return s;

  auto fm = std::make_shared<FileMeta>();
  fm->number = number;
  fm->smallest = reader->smallest_key();
  fm->largest = reader->largest_key();
  fm->file_size = w.FileSize();
  fm->num_entries = w.NumEntries();
  fm->reader = std::move(reader);
  flush_bytes_ += fm->file_size;

  {
    std::lock_guard<std::mutex> lk(mu_);
    auto nv = std::make_shared<Version>(*current_);
    if (nv->levels.empty()) nv->levels.resize(1);
    nv->levels[0].push_back(fm);  // newest L0 file at the back
    current_ = nv;
    Status ms = WriteManifest(nv);
    if (!ms.ok()) return ms;

    // The flushed data is now durable in the SSTable; reset the memtable and
    // start a fresh WAL.
    mem_.reset(new SkipList());
    wal_.Close();
    int fd = ::open(wal_path_.c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0644);
    if (fd >= 0) ::close(fd);
    Status ws = wal_.Open(wal_path_);
    if (!ws.ok()) return ws;

    bg_cv_.notify_all();
  }
  return Status::OK();
}

// ---- reads ------------------------------------------------------------------

LookupResult DB::Get(const std::string& key, std::string* value) const {
  // Memtable is the newest data and is touched only by the writer thread.
  LookupResult r = mem_->Get(key, value);
  if (r != LookupResult::kNotFound) return r;

  VersionPtr v;
  {
    std::lock_guard<std::mutex> lk(mu_);
    v = current_;
  }
  return GetFromVersion(v, key, value);
}

LookupResult DB::GetFromVersion(const VersionPtr& v, const std::string& key,
                                std::string* value) const {
  if (!v || v->levels.empty()) return LookupResult::kNotFound;

  // L0: files overlap, so scan newest (highest number, at the back) first.
  const auto& l0 = v->levels[0];
  for (auto it = l0.rbegin(); it != l0.rend(); ++it) {
    const FileMeta& f = **it;
    if (key < f.smallest || key > f.largest) continue;
    Status st;
    LookupResult r = f.reader->Get(key, value, &st);
    if (r != LookupResult::kNotFound) return r;
  }

  // Deeper levels are non-overlapping: binary-search for the one candidate file.
  for (size_t lvl = 1; lvl < v->levels.size(); ++lvl) {
    const auto& files = v->levels[lvl];
    if (files.empty()) continue;
    // Last file whose smallest <= key.
    int lo = 0, hi = static_cast<int>(files.size());
    while (lo < hi) {
      int mid = (lo + hi) / 2;
      if (files[mid]->smallest <= key) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    int idx = lo - 1;
    if (idx < 0) continue;
    const FileMeta& f = *files[idx];
    if (key > f.largest) continue;
    Status st;
    LookupResult r = f.reader->Get(key, value, &st);
    if (r != LookupResult::kNotFound) return r;
  }
  return LookupResult::kNotFound;
}

// ---- compaction -------------------------------------------------------------

bool DB::NeedsCompaction(const VersionPtr& v) const {
  if (!v || v->levels.empty()) return false;
  if (static_cast<int>(v->levels[0].size()) >= options_.l0_compaction_trigger) {
    return true;
  }
  for (size_t i = 1; i < v->levels.size() && i < (size_t)options_.max_levels - 1;
       ++i) {
    uint64_t budget = options_.l1_target_bytes;
    for (size_t j = 1; j < i; ++j) budget *= options_.level_size_multiplier;
    uint64_t bytes = 0;
    for (const auto& f : v->levels[i]) bytes += f->file_size;
    if (!v->levels[i].empty() && bytes > budget) return true;
  }
  return false;
}

bool DB::PickCompaction(const VersionPtr& v, Compaction* c) const {
  if (!v || v->levels.empty()) return false;

  auto no_data_below = [&](int target) {
    for (size_t lvl = target + 1; lvl < v->levels.size(); ++lvl) {
      if (!v->levels[lvl].empty()) return false;
    }
    return true;
  };

  // Priority 1: too many L0 files. Merge all of L0 (they mutually overlap) plus
  // the overlapping L1 files.
  if (static_cast<int>(v->levels[0].size()) >= options_.l0_compaction_trigger) {
    c->source_level = 0;
    c->target_level = 1;
    std::vector<FileMetaPtr> l0 = v->levels[0];
    // Newest first for priority.
    std::sort(l0.begin(), l0.end(),
              [](const FileMetaPtr& a, const FileMetaPtr& b) {
                return a->number > b->number;
              });
    std::string lo = l0.front()->smallest, hi = l0.front()->largest;
    for (const auto& f : l0) {
      lo = std::min(lo, f->smallest);
      hi = std::max(hi, f->largest);
    }
    for (const auto& f : l0) c->inputs.push_back(f);
    if (v->levels.size() > 1) {
      for (const auto& f : v->levels[1]) {
        if (Overlaps(f->smallest, f->largest, lo, hi)) c->inputs.push_back(f);
      }
    }
    c->bottommost = no_data_below(1);
    return true;
  }

  // Priority 2: a deeper level is over its byte budget. Pick one file from it
  // and merge with the overlapping files in the next level.
  for (size_t i = 1; i < v->levels.size() && i < (size_t)options_.max_levels - 1;
       ++i) {
    uint64_t budget = options_.l1_target_bytes;
    for (size_t j = 1; j < i; ++j) budget *= options_.level_size_multiplier;
    uint64_t bytes = 0;
    for (const auto& f : v->levels[i]) bytes += f->file_size;
    if (v->levels[i].empty() || bytes <= budget) continue;

    c->source_level = static_cast<int>(i);
    c->target_level = static_cast<int>(i + 1);
    FileMetaPtr chosen = v->levels[i].front();  // simple, deterministic pick
    c->inputs.push_back(chosen);
    if (v->levels.size() > i + 1) {
      for (const auto& f : v->levels[i + 1]) {
        if (Overlaps(f->smallest, f->largest, chosen->smallest,
                     chosen->largest)) {
          c->inputs.push_back(f);
        }
      }
    }
    c->bottommost = no_data_below(static_cast<int>(i + 1));
    return true;
  }
  return false;
}

Status DB::DoCompaction(const Compaction& c) {
  std::vector<SSTableReader::Iterator> children;
  children.reserve(c.inputs.size());
  for (const auto& f : c.inputs) children.push_back(f->reader->NewIterator());
  MergingIterator merge(std::move(children));

  std::vector<FileMetaPtr> outputs;
  std::unique_ptr<SSTableWriter> w;
  uint64_t cur_number = 0;
  std::string cur_path;

  auto finish_output = [&]() -> Status {
    if (!w) return Status::OK();
    Status s = w->Finish();
    if (!s.ok()) return s;
    std::unique_ptr<SSTableReader> reader;
    s = SSTableReader::Open(cur_path, &reader);
    if (!s.ok()) return s;
    auto fm = std::make_shared<FileMeta>();
    fm->number = cur_number;
    fm->smallest = reader->smallest_key();
    fm->largest = reader->largest_key();
    fm->file_size = w->FileSize();
    fm->num_entries = w->NumEntries();
    fm->reader = std::move(reader);
    compaction_bytes_ += fm->file_size;
    outputs.push_back(std::move(fm));
    w.reset();
    return Status::OK();
  };

  for (merge.SeekToFirst(); merge.Valid(); merge.Next()) {
    // A tombstone in the bottommost level shadows nothing below it, so it can
    // be dropped entirely -- this is where deletes finally reclaim space.
    if (c.bottommost && merge.IsTombstone()) continue;

    if (!w) {
      {
        std::lock_guard<std::mutex> lk(mu_);
        cur_number = next_file_number_++;
      }
      cur_path = FilePath(cur_number);
      w.reset(new SSTableWriter(cur_path, kOutputBlockSize,
                                options_.bloom_bits_per_key));
      Status s = w->Open();
      if (!s.ok()) return s;
    }
    ValueType t =
        merge.IsTombstone() ? ValueType::kTombstone : ValueType::kValue;
    Status s = w->Add(merge.key(), merge.value(), t);
    if (!s.ok()) return s;

    if (w->FileSize() >= options_.target_file_size) {
      s = finish_output();
      if (!s.ok()) return s;
    }
  }
  if (!merge.status().ok()) return merge.status();
  Status s = finish_output();
  if (!s.ok()) return s;

  // Install the result and drop the input files.
  std::lock_guard<std::mutex> lk(mu_);
  VersionPtr nv = InstallCompaction(c, outputs);
  current_ = nv;
  Status ms = WriteManifest(nv);
  if (!ms.ok()) return ms;
  for (const auto& f : c.inputs) ::unlink(FilePath(f->number).c_str());
  return Status::OK();
}

DB::VersionPtr DB::InstallCompaction(
    const Compaction& c, const std::vector<FileMetaPtr>& outputs) const {
  auto nv = std::make_shared<Version>(*current_);
  int need = std::max(c.source_level, c.target_level) + 1;
  if (static_cast<int>(nv->levels.size()) < need) nv->levels.resize(need);

  std::set<uint64_t> removed;
  for (const auto& f : c.inputs) removed.insert(f->number);

  auto strip = [&](int lvl) {
    auto& files = nv->levels[lvl];
    files.erase(std::remove_if(files.begin(), files.end(),
                               [&](const FileMetaPtr& f) {
                                 return removed.count(f->number) > 0;
                               }),
                files.end());
  };
  strip(c.source_level);
  strip(c.target_level);

  for (const auto& o : outputs) nv->levels[c.target_level].push_back(o);

  // Keep deeper levels sorted by key so they stay non-overlapping and
  // binary-searchable.
  if (c.target_level >= 1) {
    auto& files = nv->levels[c.target_level];
    std::sort(files.begin(), files.end(),
              [](const FileMetaPtr& a, const FileMetaPtr& b) {
                return a->smallest < b->smallest;
              });
  }
  return nv;
}

void DB::BackgroundLoop() {
  std::unique_lock<std::mutex> lk(mu_);
  while (!shutting_down_) {
    if (!NeedsCompaction(current_)) {
      bg_busy_ = false;
      idle_cv_.notify_all();
      bg_cv_.wait(lk);
      continue;
    }
    Compaction c;
    if (!PickCompaction(current_, &c)) {
      bg_busy_ = false;
      idle_cv_.notify_all();
      bg_cv_.wait(lk);
      continue;
    }
    bg_busy_ = true;
    lk.unlock();
    Status s = DoCompaction(c);  // installs the result under mu_ internally
    lk.lock();
    (void)s;  // a failed compaction leaves the old version in place; retry later
  }
  bg_busy_ = false;
  idle_cv_.notify_all();
}

void DB::TEST_WaitForCompactions() {
  std::unique_lock<std::mutex> lk(mu_);
  while (bg_busy_ || NeedsCompaction(current_)) {
    bg_cv_.notify_all();  // ensure the worker is awake
    idle_cv_.wait_for(lk, std::chrono::milliseconds(50));
  }
}

int DB::TEST_NumLevelsWithData() const {
  std::lock_guard<std::mutex> lk(mu_);
  int n = 0;
  for (const auto& level : current_->levels) {
    if (!level.empty()) ++n;
  }
  return n;
}

DB::Stats DB::GetStats() const {
  Stats s;
  s.user_bytes = user_bytes_.load();
  s.flush_bytes = flush_bytes_.load();
  s.compaction_bytes = compaction_bytes_.load();
  s.sstable_bytes_written = s.flush_bytes + s.compaction_bytes;
  s.write_amplification =
      s.user_bytes ? static_cast<double>(s.sstable_bytes_written) / s.user_bytes
                   : 0.0;
  std::lock_guard<std::mutex> lk(mu_);
  for (const auto& level : current_->levels) {
    int files = static_cast<int>(level.size());
    uint64_t bytes = 0;
    for (const auto& f : level) bytes += f->file_size;
    s.files_per_level.push_back(files);
    s.bytes_per_level.push_back(bytes);
  }
  return s;
}

}  // namespace lsmdb
