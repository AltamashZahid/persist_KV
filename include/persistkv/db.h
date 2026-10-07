#pragma once

#include <string>

#include "persistkv/btree.h"
#include "persistkv/common.h"
#include "persistkv/pager.h"
#include "persistkv/wal.h"
#include "persistkv/write_batch.h"

namespace pkv {

struct Options {
  // fsync the WAL on every write. With this off, writes survive a process
  // crash (they are in the OS page cache) but not a power failure.
  bool sync_writes = true;
  size_t cache_pages = 4096;
  // A checkpoint runs when either threshold is crossed.
  uint64_t checkpoint_wal_bytes = 8ull << 20;
  size_t checkpoint_dirty_pages = 2048;
};

struct DBStats {
  uint64_t keys = 0;
  uint32_t height = 0;
  uint32_t pages = 0;
  size_t dirty_pages = 0;
  size_t cached_pages = 0;
  uint64_t wal_bytes = 0;
  Lsn last_lsn = 0;
  uint64_t checkpoints = 0;
  uint64_t cache_hits = 0;
  uint64_t cache_misses = 0;
  // What recovery did when this DB was opened.
  uint64_t wal_records_replayed = 0;
  uint64_t wal_bytes_truncated = 0;
  bool recovered_from_doublewrite = false;
};

// A crash-safe key-value store.
//
// Write path: append to the WAL (fsync) -> apply to the B+Tree in the page
// cache -> occasionally checkpoint dirty pages to the data file.
// Open path: repair from the doublewrite buffer -> load meta -> replay WAL
// records newer than the last checkpoint.
//
// If an operation fails midway (an I/O error, or corruption found while
// applying a change), the in-memory tree may be half-modified. The DB then
// enters a failed state: every later call throws, and nothing more is
// written to the data file. Reopening recovers from the WAL.
class DB {
 public:
  static constexpr const char* kDataFile = "data.db";
  static constexpr const char* kWalFile = "wal.log";
  static constexpr const char* kDoublewriteFile = "dwb.log";

  explicit DB(const std::string& dir, const Options& options = Options());
  ~DB();  // checkpoints, so a clean reopen needs no replay
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  // Keys must be 1..kMaxKeySize bytes, values 0..kMaxValueSize bytes.
  void put(const std::string& key, const std::string& value);
  bool get(const std::string& key, std::string* value);
  bool remove(const std::string& key);

  // Applies every operation in the batch atomically, with one fsync.
  void write(const WriteBatch& batch);

  // Visits keys in [lo, hi] in order; an empty `hi` means no upper bound.
  // Stop early by returning false from fn.
  void scan(const std::string& lo, const std::string& hi, const ScanFn& fn);

  void checkpoint();
  uint64_t size() const { return pager_.meta().key_count; }
  std::string checkIntegrity();
  DBStats stats();

  // Testing hook: makes the destructor skip its checkpoint, so the files are
  // left exactly as a crash at this moment would leave them.
  void abandonForTesting() { abandoned_ = true; }

  // Deletes the database files in `dir` (the directory itself is kept).
  static void destroy(const std::string& dir);

 private:
  static void validate(const std::string& key, const std::string* value);
  void ensureUsable() const;
  void commit(const WriteBatch& batch);
  void applyBatch(const WriteBatch& batch);
  void maybeCheckpoint();

  std::string dir_;
  Options options_;
  Pager pager_;
  Wal wal_;
  BTree tree_;
  Lsn last_lsn_ = 0;
  uint64_t replayed_ = 0;
  bool abandoned_ = false;
  bool failed_ = false;
};

}  // namespace pkv
