#include "lsmdb/wal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "lsmdb/coding.h"
#include "lsmdb/crc32.h"

namespace lsmdb {
namespace {

constexpr size_t kHeaderSize = 4 + 4;  // crc + length

Status WriteFully(int fd, const char* data, size_t n, const std::string& path) {
  size_t off = 0;
  while (off < n) {
    ssize_t r = ::write(fd, data + off, n - off);
    if (r < 0) {
      if (errno == EINTR) continue;
      return Status::IOError("write " + path + ": " + strerror(errno));
    }
    off += static_cast<size_t>(r);
  }
  return Status::OK();
}

}  // namespace

WalWriter::~WalWriter() {
  if (fd_ >= 0) ::close(fd_);
}

Status WalWriter::Open(const std::string& path) {
  path_ = path;
  fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd_ < 0) return Status::IOError("open " + path + ": " + strerror(errno));
  return Status::OK();
}

Status WalWriter::AddRecord(ValueType type, const std::string& key,
                            const std::string& value, bool sync) {
  std::string payload;
  payload.push_back(static_cast<char>(type));
  PutFixed32(&payload, static_cast<uint32_t>(key.size()));
  payload.append(key);
  PutFixed32(&payload, static_cast<uint32_t>(value.size()));
  payload.append(value);

  std::string frame;
  PutFixed32(&frame, Crc32(payload));
  PutFixed32(&frame, static_cast<uint32_t>(payload.size()));
  frame.append(payload);

  Status s = WriteFully(fd_, frame.data(), frame.size(), path_);
  if (!s.ok()) return s;

  if (sync) {
    // fsync is the durability point: the write is not acked until its bytes are
    // on stable storage.
    if (::fsync(fd_) != 0) {
      return Status::IOError("fsync " + path_ + ": " + strerror(errno));
    }
  }
  return Status::OK();
}

Status WalWriter::Close() {
  if (fd_ >= 0) {
    if (::close(fd_) != 0) {
      fd_ = -1;
      return Status::IOError("close " + path_ + ": " + strerror(errno));
    }
    fd_ = -1;
  }
  return Status::OK();
}

Status WalReplay(
    const std::string& path,
    const std::function<void(ValueType, const std::string&,
                             const std::string&)>& handler,
    size_t* num_replayed) {
  if (num_replayed) *num_replayed = 0;

  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) return Status::OK();  // fresh DB, no log yet
    return Status::IOError("open " + path + ": " + strerror(errno));
  }
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return Status::IOError("fstat " + path + ": " + strerror(errno));
  }
  size_t size = static_cast<size_t>(st.st_size);

  std::string buf;
  buf.resize(size);
  size_t off = 0;
  while (off < size) {
    ssize_t r = ::read(fd, &buf[off], size - off);
    if (r < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return Status::IOError("read " + path + ": " + strerror(errno));
    }
    if (r == 0) break;
    off += static_cast<size_t>(r);
  }
  ::close(fd);
  buf.resize(off);

  const char* p = buf.data();
  size_t pos = 0;
  size_t count = 0;
  while (pos + kHeaderSize <= buf.size()) {
    uint32_t crc = DecodeFixed32(p + pos);
    uint32_t len = DecodeFixed32(p + pos + 4);
    if (pos + kHeaderSize + len > buf.size()) break;  // torn tail record
    const char* payload = p + pos + kHeaderSize;
    if (Crc32(payload, len) != crc) break;  // corrupt tail record

    // Decode payload. Guard every field against a malformed length.
    if (len < 1 + 4) break;
    size_t q = 0;
    ValueType type = static_cast<ValueType>(static_cast<unsigned char>(payload[q]));
    q += 1;
    uint32_t klen = DecodeFixed32(payload + q);
    q += 4;
    if (q + klen + 4 > len) break;
    std::string key(payload + q, klen);
    q += klen;
    uint32_t vlen = DecodeFixed32(payload + q);
    q += 4;
    if (q + vlen != len) break;
    std::string value(payload + q, vlen);

    handler(type, key, value);
    ++count;
    pos += kHeaderSize + len;
  }

  if (num_replayed) *num_replayed = count;
  return Status::OK();
}

}  // namespace lsmdb
