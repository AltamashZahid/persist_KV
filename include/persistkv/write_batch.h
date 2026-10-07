#pragma once

#include <string>
#include <vector>

#include "persistkv/common.h"

namespace pkv {

enum class WalOp : uint8_t { Put = 1, Delete = 2 };

// A group of puts and deletes that DB::write() commits atomically: after a
// crash, either every operation in the batch is present or none is. The
// whole batch costs one WAL record and one fsync.
class WriteBatch {
 public:
  struct Entry {
    WalOp op;
    std::string key;
    std::string value;
  };

  void put(const std::string& key, const std::string& value) {
    entries_.push_back({WalOp::Put, key, value});
    bytes_ += key.size() + value.size();
  }
  void remove(const std::string& key) {
    entries_.push_back({WalOp::Delete, key, std::string()});
    bytes_ += key.size();
  }
  void clear() {
    entries_.clear();
    bytes_ = 0;
  }

  size_t count() const { return entries_.size(); }
  bool empty() const { return entries_.empty(); }
  size_t byteSize() const { return bytes_; }  // sum of key and value lengths
  const std::vector<Entry>& entries() const { return entries_; }

 private:
  std::vector<Entry> entries_;
  size_t bytes_ = 0;
};

}  // namespace pkv
