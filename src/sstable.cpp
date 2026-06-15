#include "lsmdb/sstable.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "lsmdb/coding.h"
#include "lsmdb/crc32.h"

namespace lsmdb {
namespace {

// One on-disk record header: type(1) + key_len(4) + value_len(4).
constexpr size_t kRecordHeader = 1 + 4 + 4;

void AppendRecord(std::string* buf, const std::string& key,
                  const std::string& value, ValueType type) {
  buf->push_back(static_cast<char>(type));
  PutFixed32(buf, static_cast<uint32_t>(key.size()));
  PutFixed32(buf, static_cast<uint32_t>(value.size()));
  buf->append(key);
  buf->append(value);
}

}  // namespace

// ===== Writer ================================================================

SSTableWriter::SSTableWriter(std::string path, size_t block_size,
                             int bloom_bits_per_key)
    : path_(std::move(path)),
      block_size_(block_size),
      bloom_(bloom_bits_per_key) {}

SSTableWriter::~SSTableWriter() {
  if (fd_ >= 0) ::close(fd_);
}

Status SSTableWriter::Open() {
  fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd_ < 0) return Status::IOError("open " + path_ + ": " + strerror(errno));
  return Status::OK();
}

Status SSTableWriter::WriteRaw(const char* data, size_t n) {
  size_t written = 0;
  while (written < n) {
    ssize_t r = ::write(fd_, data + written, n - written);
    if (r < 0) {
      if (errno == EINTR) continue;
      return Status::IOError("write " + path_ + ": " + strerror(errno));
    }
    written += static_cast<size_t>(r);
  }
  return Status::OK();
}

Status SSTableWriter::Add(const std::string& key, const std::string& value,
                          ValueType type) {
  if (block_buf_.empty()) block_first_key_ = key;
  AppendRecord(&block_buf_, key, value, type);
  bloom_.Add(key);
  ++num_entries_;
  if (block_buf_.size() >= block_size_) return FlushBlock();
  return Status::OK();
}

Status SSTableWriter::FlushBlock() {
  if (block_buf_.empty()) return Status::OK();
  const uint64_t block_offset = offset_;
  const uint32_t block_len = static_cast<uint32_t>(block_buf_.size());

  Status s = WriteRaw(block_buf_.data(), block_buf_.size());
  if (!s.ok()) return s;

  std::string crc_buf;
  PutFixed32(&crc_buf, Crc32(block_buf_));
  s = WriteRaw(crc_buf.data(), crc_buf.size());
  if (!s.ok()) return s;

  offset_ += block_len + 4;
  index_.push_back({block_first_key_, block_offset, block_len});
  block_buf_.clear();
  return Status::OK();
}

Status SSTableWriter::Finish() {
  if (finished_) return Status::OK();
  finished_ = true;

  Status s = FlushBlock();
  if (!s.ok()) return s;

  // Filter block: a bloom filter over every key, checksummed like a data block.
  std::string filter_buf = bloom_.Finish();
  const uint64_t filter_offset = offset_;
  const uint64_t filter_length = filter_buf.size();
  s = WriteRaw(filter_buf.data(), filter_buf.size());
  if (!s.ok()) return s;
  {
    std::string crc_buf;
    PutFixed32(&crc_buf, Crc32(filter_buf));
    s = WriteRaw(crc_buf.data(), crc_buf.size());
    if (!s.ok()) return s;
  }
  offset_ += filter_length + 4;

  // Index block: one length-prefixed first_key + offset + length per data block.
  std::string index_buf;
  for (const auto& e : index_) {
    PutLengthPrefixed(&index_buf, e.first_key);
    PutFixed64(&index_buf, e.offset);
    PutFixed32(&index_buf, e.length);
  }
  const uint64_t index_offset = offset_;
  const uint64_t index_length = index_buf.size();

  s = WriteRaw(index_buf.data(), index_buf.size());
  if (!s.ok()) return s;
  std::string crc_buf;
  PutFixed32(&crc_buf, Crc32(index_buf));
  s = WriteRaw(crc_buf.data(), crc_buf.size());
  if (!s.ok()) return s;
  offset_ += index_length + 4;

  // Footer.
  std::string footer;
  PutFixed64(&footer, filter_offset);
  PutFixed64(&footer, filter_length);
  PutFixed64(&footer, index_offset);
  PutFixed64(&footer, index_length);
  PutFixed64(&footer, num_entries_);
  PutFixed32(&footer, kSSTableMagic);
  s = WriteRaw(footer.data(), footer.size());
  if (!s.ok()) return s;
  offset_ += footer.size();

  if (::fsync(fd_) != 0) {
    return Status::IOError("fsync " + path_ + ": " + strerror(errno));
  }
  if (::close(fd_) != 0) {
    fd_ = -1;
    return Status::IOError("close " + path_ + ": " + strerror(errno));
  }
  fd_ = -1;
  return Status::OK();
}

Status FlushMemTableToSSTable(const SkipList& mem, const std::string& path,
                              uint64_t* file_size, uint64_t* num_entries) {
  SSTableWriter writer(path);
  Status s = writer.Open();
  if (!s.ok()) return s;
  auto it = mem.NewIterator();
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    ValueType t = it.IsTombstone() ? ValueType::kTombstone : ValueType::kValue;
    s = writer.Add(it.key(), it.value(), t);
    if (!s.ok()) return s;
  }
  s = writer.Finish();
  if (!s.ok()) return s;
  if (file_size) *file_size = writer.FileSize();
  if (num_entries) *num_entries = writer.NumEntries();
  return Status::OK();
}

// ===== Reader ================================================================

SSTableReader::~SSTableReader() {
  if (base_ != nullptr) {
    ::munmap(const_cast<char*>(base_), file_size_);
  }
}

Status SSTableReader::Open(const std::string& path,
                           std::unique_ptr<SSTableReader>* out,
                           BlockCache* cache, uint64_t file_number) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return Status::IOError("open " + path + ": " + strerror(errno));

  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return Status::IOError("fstat " + path + ": " + strerror(errno));
  }
  size_t size = static_cast<size_t>(st.st_size);
  if (size < kFooterSize) {
    ::close(fd);
    return Status::Corruption("file too small to be an SSTable: " + path);
  }

  void* addr = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);  // mapping stays valid after close
  if (addr == MAP_FAILED) {
    return Status::IOError("mmap " + path + ": " + strerror(errno));
  }

  auto reader = std::unique_ptr<SSTableReader>(new SSTableReader());
  reader->base_ = static_cast<const char*>(addr);
  reader->file_size_ = size;
  reader->cache_ = cache;
  reader->file_number_ = file_number;

  const char* footer = reader->base_ + size - kFooterSize;
  uint64_t filter_offset = DecodeFixed64(footer);
  uint64_t filter_length = DecodeFixed64(footer + 8);
  uint64_t index_offset = DecodeFixed64(footer + 16);
  uint64_t index_length = DecodeFixed64(footer + 24);
  uint64_t num_entries = DecodeFixed64(footer + 32);
  uint32_t magic = DecodeFixed32(footer + 40);
  if (magic != kSSTableMagic) {
    return Status::Corruption("bad SSTable magic in " + path);
  }
  if (index_offset + index_length + 4 > size ||
      filter_offset + filter_length + 4 > size) {
    return Status::Corruption("metadata extends past EOF in " + path);
  }

  // Verify the index CRC before trusting any offsets it hands out.
  const char* index_data = reader->base_ + index_offset;
  uint32_t stored_crc = DecodeFixed32(index_data + index_length);
  if (Crc32(index_data, index_length) != stored_crc) {
    return Status::Corruption("index CRC mismatch in " + path);
  }

  const char* p = index_data;
  const char* end = index_data + index_length;
  while (p < end) {
    if (p + 4 > end) return Status::Corruption("truncated index entry");
    uint32_t klen = DecodeFixed32(p);
    p += 4;
    if (p + klen + 12 > end) return Status::Corruption("truncated index entry");
    IndexEntry e;
    e.first_key.assign(p, klen);
    p += klen;
    e.offset = DecodeFixed64(p);
    p += 8;
    e.length = DecodeFixed32(p);
    p += 4;
    reader->index_.push_back(std::move(e));
  }
  reader->num_entries_ = num_entries;

  // Load the bloom filter block (CRC-verified) into memory.
  if (filter_length > 0) {
    const char* fdata = reader->base_ + filter_offset;
    uint32_t stored_crc = DecodeFixed32(fdata + filter_length);
    if (Crc32(fdata, filter_length) != stored_crc) {
      return Status::Corruption("filter CRC mismatch in " + path);
    }
    reader->filter_.Reset(std::string(fdata, filter_length));
  }

  // Derive smallest/largest key (used later for compaction overlap checks).
  if (!reader->index_.empty()) {
    reader->smallest_key_ = reader->index_.front().first_key;
    std::string last_block;
    Status s = reader->ReadBlock(reader->index_.size() - 1, &last_block);
    if (!s.ok()) return s;
    const char* q = last_block.data();
    const char* qend = q + last_block.size();
    while (q < qend) {
      q += 1;  // type
      uint32_t klen = DecodeFixed32(q);
      q += 4;
      uint32_t vlen = DecodeFixed32(q);
      q += 4;
      reader->largest_key_.assign(q, klen);
      q += klen + vlen;
    }
  }

  // The last-block scan above counts as internal bookkeeping, not a user read.
  reader->blocks_read_ = 0;

  *out = std::move(reader);
  return Status::OK();
}

Status SSTableReader::ReadBlock(size_t block_idx, std::string* out) const {
  const IndexEntry& e = index_[block_idx];
  if (e.offset + e.length + 4 > file_size_) {
    return Status::Corruption("block extends past EOF");
  }
  const char* data = base_ + e.offset;
  uint32_t stored_crc = DecodeFixed32(data + e.length);
  if (Crc32(data, e.length) != stored_crc) {
    return Status::Corruption("block CRC mismatch");
  }
  out->assign(data, e.length);
  ++blocks_read_;
  return Status::OK();
}

Status SSTableReader::GetBlock(size_t block_idx, BlockPtr* out) const {
  const uint64_t offset = index_[block_idx].offset;
  if (cache_ != nullptr) {
    BlockPtr cached = cache_->Lookup(file_number_, offset);
    if (cached) {  // hit: skip the copy + CRC recompute entirely
      *out = std::move(cached);
      return Status::OK();
    }
  }
  auto block = std::make_shared<std::string>();
  Status s = ReadBlock(block_idx, block.get());  // copies + verifies CRC
  if (!s.ok()) return s;
  BlockPtr bp = std::move(block);
  if (cache_ != nullptr) cache_->Insert(file_number_, offset, bp);
  *out = std::move(bp);
  return Status::OK();
}

LookupResult SSTableReader::Get(const std::string& key, std::string* value,
                                Status* status) const {
  if (status) *status = Status::OK();
  if (index_.empty()) return LookupResult::kNotFound;

  // Bloom short-circuit: if the filter rules the key out, skip the block read
  // entirely. This is the O(1) "definitely not here" that avoids disk I/O.
  if (filter_enabled_ && filter_.valid() && !filter_.MayContain(key)) {
    return LookupResult::kNotFound;
  }

  // Find the last block whose first_key <= key.
  auto it = std::upper_bound(
      index_.begin(), index_.end(), key,
      [](const std::string& k, const IndexEntry& e) { return k < e.first_key; });
  if (it == index_.begin()) return LookupResult::kNotFound;  // key < everything
  size_t block_idx = static_cast<size_t>((it - index_.begin()) - 1);

  BlockPtr block;
  Status s = GetBlock(block_idx, &block);
  if (!s.ok()) {
    if (status) *status = s;
    return LookupResult::kNotFound;
  }

  const char* p = block->data();
  const char* end = p + block->size();
  while (p < end) {
    ValueType type = static_cast<ValueType>(static_cast<unsigned char>(*p));
    p += 1;
    uint32_t klen = DecodeFixed32(p);
    p += 4;
    uint32_t vlen = DecodeFixed32(p);
    p += 4;
    const char* kptr = p;
    p += klen;
    const char* vptr = p;
    p += vlen;

    int cmp = key.compare(0, key.size(), kptr, klen);
    if (cmp == 0) {
      if (type == ValueType::kTombstone) return LookupResult::kDeleted;
      if (value) value->assign(vptr, vlen);
      return LookupResult::kFound;
    }
    if (cmp < 0) break;  // records sorted; target would have appeared already
  }
  return LookupResult::kNotFound;
}

// ---- Reader::Iterator -------------------------------------------------------

SSTableReader::Iterator::Iterator(const SSTableReader* reader)
    : reader_(reader) {}

void SSTableReader::Iterator::LoadBlock(size_t block_idx) {
  block_idx_ = block_idx;
  pos_ = 0;
  status_ = reader_->ReadBlock(block_idx, &block_);
}

void SSTableReader::Iterator::ParseCurrent() {
  // Advance across empty/exhausted blocks until a record is found or we run out.
  while (true) {
    if (!status_.ok()) {
      valid_ = false;
      return;
    }
    if (pos_ >= block_.size()) {
      if (block_idx_ + 1 >= reader_->index_.size()) {
        valid_ = false;
        return;
      }
      LoadBlock(block_idx_ + 1);
      continue;
    }
    const char* p = block_.data() + pos_;
    type_ = static_cast<ValueType>(static_cast<unsigned char>(*p));
    p += 1;
    uint32_t klen = DecodeFixed32(p);
    p += 4;
    uint32_t vlen = DecodeFixed32(p);
    p += 4;
    key_.assign(p, klen);
    p += klen;
    value_.assign(p, vlen);
    p += vlen;
    pos_ = static_cast<size_t>(p - block_.data());
    valid_ = true;
    return;
  }
}

void SSTableReader::Iterator::SeekToFirst() {
  if (reader_->index_.empty()) {
    valid_ = false;
    return;
  }
  LoadBlock(0);
  ParseCurrent();
}

void SSTableReader::Iterator::Seek(const std::string& target) {
  if (reader_->index_.empty()) {
    valid_ = false;
    return;
  }
  // Binary-search the sparse index for the block that could contain target:
  // the last block whose first key is <= target (or the first block if target
  // precedes everything).
  const auto& index = reader_->index_;
  int lo = 0, hi = static_cast<int>(index.size());
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (index[mid].first_key <= target) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  size_t idx = (lo == 0) ? 0 : static_cast<size_t>(lo - 1);

  LoadBlock(idx);
  // Scan forward (crossing blocks if needed) to the first key >= target.
  while (true) {
    ParseCurrent();
    if (!valid_) return;
    if (key_ >= target) return;
  }
}

void SSTableReader::Iterator::Next() {
  if (!valid_) return;
  ParseCurrent();
}

}  // namespace lsmdb
