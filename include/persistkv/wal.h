#pragma once

#include <functional>
#include <string>

#include "persistkv/common.h"
#include "persistkv/file.h"

namespace pkv {

enum class WalOp : uint8_t { Put = 1, Delete = 2 };

struct WalRecord {
  Lsn lsn = 0;
  WalOp op = WalOp::Put;
  std::string key;
  std::string value;
};

// Append-only redo log. On-disk record layout:
//   [crc32 u32][len u32][lsn u64][op u8][klen u16][vlen u16][key][value]
// where len counts the bytes after the len field and the CRC covers
// everything after the CRC field. Records are logical (put/delete), so
// replaying them on top of the last checkpoint is idempotent.
class Wal {
 public:
  explicit Wal(std::string path) : path_(std::move(path)) {}

  void open();

  // Calls fn for every intact record in order. Stops at the first torn or
  // corrupt record and truncates the file there. Returns the last LSN seen.
  Lsn replay(const std::function<void(const WalRecord&)>& fn);

  void append(const WalRecord& rec, bool sync);
  void reset();  // empty the log after a checkpoint

  uint64_t size() const { return size_; }
  uint64_t truncatedBytes() const { return truncated_bytes_; }

 private:
  std::string path_;
  File file_;
  uint64_t size_ = 0;
  uint64_t truncated_bytes_ = 0;
};

}  // namespace pkv
