#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "persistkv/common.h"
#include "persistkv/file.h"
#include "persistkv/write_batch.h"

namespace pkv {

// Largest batch (sum of key and value bytes) one WAL record may carry.
constexpr uint64_t kMaxBatchBytes = 64ull << 20;

// Append-only redo log. Each record holds one atomic batch:
//   [crc32 u32][len u32][lsn u64][count u32]
//   then `count` times: [op u8][klen u16][vlen u32][key][value]
// where len counts the bytes after the len field and the CRC covers
// everything after the CRC field. A record is applied entirely or (if torn)
// not at all, which is what makes a WriteBatch atomic. Operations are
// logical (put/delete), so replaying them on top of the last checkpoint is
// idempotent.
class Wal {
 public:
  using ReplayFn = std::function<void(Lsn lsn, const WriteBatch& batch)>;

  explicit Wal(std::string path) : path_(std::move(path)) {}

  void open();

  // Calls fn for every intact record in order. Stops at the first torn or
  // corrupt record and truncates the file there. Returns the last LSN seen.
  Lsn replay(const ReplayFn& fn);

  void append(Lsn lsn, const WriteBatch& batch, bool sync);
  void sync();   // make every appended record durable
  void reset();  // empty the log after a checkpoint

  uint64_t size() const { return size_; }
  uint64_t truncatedBytes() const { return truncated_bytes_; }

 private:
  std::string path_;
  File file_;
  // Atomic because a checkpoint resets the retired log without the DB's WAL
  // lock while stats() may be reading its size.
  std::atomic<uint64_t> size_{0};
  uint64_t truncated_bytes_ = 0;
};

}  // namespace pkv
