#pragma once

// Minimal Status type used across the engine to report success or a typed
// failure without exceptions on the hot path.

#include <string>
#include <utility>

namespace lsmdb {

class Status {
 public:
  enum class Code { kOk, kNotFound, kCorruption, kIOError, kInvalidArgument };

  Status() : code_(Code::kOk) {}

  static Status OK() { return Status(); }
  static Status NotFound(std::string msg = "") {
    return Status(Code::kNotFound, std::move(msg));
  }
  static Status Corruption(std::string msg) {
    return Status(Code::kCorruption, std::move(msg));
  }
  static Status IOError(std::string msg) {
    return Status(Code::kIOError, std::move(msg));
  }
  static Status InvalidArgument(std::string msg) {
    return Status(Code::kInvalidArgument, std::move(msg));
  }

  bool ok() const { return code_ == Code::kOk; }
  bool IsNotFound() const { return code_ == Code::kNotFound; }
  bool IsCorruption() const { return code_ == Code::kCorruption; }
  Code code() const { return code_; }

  std::string ToString() const {
    switch (code_) {
      case Code::kOk:
        return "OK";
      case Code::kNotFound:
        return "NotFound: " + message_;
      case Code::kCorruption:
        return "Corruption: " + message_;
      case Code::kIOError:
        return "IOError: " + message_;
      case Code::kInvalidArgument:
        return "InvalidArgument: " + message_;
    }
    return "Unknown";
  }

 private:
  Status(Code code, std::string msg) : code_(code), message_(std::move(msg)) {}
  Code code_;
  std::string message_;
};

}  // namespace lsmdb
