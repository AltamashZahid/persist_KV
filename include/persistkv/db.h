#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "persistkv/btree.h"
#include "persistkv/common.h"
#include "persistkv/pager.h"
#include "persistkv/sync.h"
#include "persistkv/wal.h"
#include "persistkv/write_batch.h"

namespace pkv {

struct Options {
  // fsync the WAL on every commit. With this off, writes survive a process
  // crash (they are in the OS page cache) but not a power failure.
  bool sync_writes = true;
  size_t cache_pages = 4096;
  // A checkpoint is triggered when either threshold is crossed. Writers
  // block (backpressure) if the backlog reaches 4x the threshold.
  uint64_t checkpoint_wal_bytes = 8ull << 20;
  size_t checkpoint_dirty_pages = 2048;
  // Run checkpoints on a background thread so writers do not wait for the
  // checkpoint's disk I/O. When false, the writer that crosses a threshold
  // runs the checkpoint itself.
  bool background_checkpoints = true;
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
  uint64_t commit_groups = 0;   // WAL fsyncs issued by group commit
  uint64_t commit_batches = 0;  // batches committed through those groups
  // What recovery did when this DB was opened.
  uint64_t wal_records_replayed = 0;
  uint64_t wal_bytes_truncated = 0;
  bool recovered_from_doublewrite = false;
};

// A crash-safe, thread-safe key-value store.
//
// Write path: concurrent writers queue up; the writer at the front (the
// leader) appends the whole group to the WAL with one fsync, applies it to
// the B+Tree in the page cache, then wakes the others (group commit).
// Readers share the tree lock and run in parallel with each other; writers
// hold it exclusively only while applying changes in memory.
//
// Checkpoints run on a background thread: under the locks they only copy the
// dirty pages and switch new WAL appends to the second log file; the slow
// doublewrite + data file I/O then happens while reads and writes continue.
//
// Open path: repair from the doublewrite buffer -> load meta -> replay both
// WAL files in LSN order, skipping records the data file already contains.
//
// If an operation fails midway (an I/O error, or corruption found while
// applying a change), the in-memory tree may be half-modified. The DB then
// enters a failed state: every later call throws, and nothing more is
// written to the data file. Reopening recovers from the WAL.
//
// Do not call DB methods from inside a scan() callback; scan holds the tree
// lock while it runs.
class DB {
 public:
  static constexpr const char* kDataFile = "data.db";
  static constexpr const char* kWalFile = "wal-0.log";
  static constexpr const char* kWalFileAlt = "wal-1.log";
  static constexpr const char* kDoublewriteFile = "dwb.log";

  explicit DB(const std::string& dir, const Options& options = Options());
  ~DB();  // stops the checkpoint thread and checkpoints, so a clean reopen needs no replay
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  // Keys must be 1..kMaxKeySize bytes, values 0..kMaxValueSize bytes.
  void put(const std::string& key, const std::string& value);
  bool get(const std::string& key, std::string* value);
  bool remove(const std::string& key);  // whether the key existed when called

  // Applies every operation in the batch atomically, with one fsync.
  void write(const WriteBatch& batch);

  // Visits keys in [lo, hi] in order; an empty `hi` means no upper bound.
  // Stop early by returning false from fn.
  void scan(const std::string& lo, const std::string& hi, const ScanFn& fn);

  void checkpoint();  // synchronous
  uint64_t size();
  std::string checkIntegrity();
  DBStats stats();

  // Testing hook: makes the destructor skip its checkpoint, so the files are
  // left exactly as a crash at this moment would leave them.
  void abandonForTesting() { abandoned_ = true; }

  // Deletes the database files in `dir` (the directory itself is kept).
  static void destroy(const std::string& dir);

 private:
  struct Writer;

  static void validate(const std::string& key, const std::string* value);
  void ensureUsable() const;
  void commit(const WriteBatch& batch);
  void writeGroup(const std::vector<Writer*>& group, bool* want_checkpoint, bool* must_wait);
  void applyBatch(const WriteBatch& batch);
  void runCheckpoint();
  void requestCheckpoint(bool wait);
  void checkpointLoop();

  std::string dir_;
  Options options_;
  Pager pager_;
  BTree tree_;
  std::unique_ptr<Wal> wals_[2];
  uint64_t replayed_ = 0;
  uint64_t truncated_ = 0;
  bool abandoned_ = false;
  std::atomic<bool> failed_;

  // Lock order: ckpt_run_mu_ -> wal_mu_ -> tree_mu_ -> (pager cache).
  Mutex ckpt_run_mu_;    // one checkpoint at a time
  Mutex wal_mu_;         // WAL appends + apply, and log rotation
  SharedMutex tree_mu_;  // readers shared, appliers and snapshots exclusive
  int active_wal_ = 0;   // guarded by wal_mu_
  Lsn last_lsn_ = 0;     // guarded by wal_mu_
  uint64_t commit_groups_ = 0;   // guarded by wal_mu_
  uint64_t commit_batches_ = 0;  // guarded by wal_mu_

  // Group commit queue.
  Mutex commit_mu_;
  CondVar commit_cv_;
  std::deque<Writer*> writers_;

  // Background checkpoint thread.
  Mutex ckpt_mu_;
  CondVar ckpt_cv_;
  bool ckpt_requested_ = false;
  bool stopping_ = false;
  uint64_t ckpt_done_ = 0;
  Thread ckpt_thread_;
};

}  // namespace pkv
