#include "persistkv/db.h"

#include <algorithm>

#include "persistkv/failpoint.h"
#include "persistkv/file.h"

namespace pkv {

namespace {

std::string joinPath(const std::string& dir, const char* name) { return dir + "/" + name; }

}  // namespace

constexpr const char* DB::kDataFile;
constexpr const char* DB::kWalFile;
constexpr const char* DB::kDoublewriteFile;

DB::DB(const std::string& dir, const Options& options)
    : dir_(dir),
      options_(options),
      pager_(joinPath(dir, kDataFile), joinPath(dir, kDoublewriteFile), options.cache_pages),
      wal_(joinPath(dir, kWalFile)),
      tree_(pager_) {
  makeDir(dir_);

  // 1. Finish or discard any interrupted checkpoint, then load the meta page.
  pager_.open();
  bool fresh = pager_.isNew();
  if (fresh) tree_.init();

  // 2. Redo every logged batch the data file has not seen yet.
  wal_.open();
  Lsn checkpointed = pager_.meta().checkpoint_lsn;
  last_lsn_ = checkpointed;
  Lsn wal_last = wal_.replay([&](Lsn lsn, const WriteBatch& batch) {
    if (lsn <= checkpointed) return;  // already in the data file
    applyBatch(batch);
    replayed_++;
  });
  last_lsn_ = std::max(last_lsn_, wal_last);

  // 3. Make the recovered state durable so the WAL can start empty.
  if (fresh || replayed_ > 0) checkpoint();
}

DB::~DB() {
  if (abandoned_ || failed_) return;  // never checkpoint a possibly half-applied tree
  try {
    checkpoint();
  } catch (...) {
    // Never throw from a destructor; the WAL still has everything.
  }
}

void DB::validate(const std::string& key, const std::string* value) {
  if (key.empty()) throw Error("key must not be empty");
  if (key.size() > kMaxKeySize) throw Error("key longer than " + std::to_string(kMaxKeySize) + " bytes");
  if (value && value->size() > kMaxValueSize) {
    throw Error("value longer than " + std::to_string(kMaxValueSize) + " bytes");
  }
}

void DB::ensureUsable() const {
  if (failed_) throw Error("database is in a failed state after an earlier error; reopen it to recover");
}

void DB::applyBatch(const WriteBatch& batch) {
  for (const WriteBatch::Entry& e : batch.entries()) {
    if (e.op == WalOp::Put) {
      tree_.put(e.key, e.value);
    } else {
      tree_.remove(e.key);
    }
  }
}

// Logs the batch, then applies it. Any failure after the log write leaves
// the tree in an unknown state, so the DB is marked failed; the batch itself
// is safe in the WAL and is replayed on the next open.
void DB::commit(const WriteBatch& batch) {
  ensureUsable();
  try {
    Lsn lsn = last_lsn_ + 1;
    wal_.append(lsn, batch, options_.sync_writes);  // durable from here on
    last_lsn_ = lsn;
    failpoint::crashIfHit("db.after_wal");
    applyBatch(batch);
    maybeCheckpoint();
  } catch (...) {
    failed_ = true;
    throw;
  }
}

void DB::put(const std::string& key, const std::string& value) {
  validate(key, &value);
  WriteBatch batch;
  batch.put(key, value);
  commit(batch);
}

bool DB::remove(const std::string& key) {
  validate(key, nullptr);
  ensureUsable();
  if (!tree_.get(key, nullptr)) return false;  // nothing to log
  WriteBatch batch;
  batch.remove(key);
  commit(batch);
  return true;
}

void DB::write(const WriteBatch& batch) {
  for (const WriteBatch::Entry& e : batch.entries()) {
    validate(e.key, e.op == WalOp::Put ? &e.value : nullptr);
  }
  if (batch.byteSize() > kMaxBatchBytes) throw Error("batch larger than the 64 MiB limit");
  if (batch.empty()) return;
  commit(batch);
}

bool DB::get(const std::string& key, std::string* value) {
  validate(key, nullptr);
  ensureUsable();
  return tree_.get(key, value);
}

void DB::scan(const std::string& lo, const std::string& hi, const ScanFn& fn) {
  ensureUsable();
  tree_.scan(lo, hi.empty() ? nullptr : &hi, fn);
}

void DB::checkpoint() {
  ensureUsable();
  try {
    pager_.checkpoint(last_lsn_);
    failpoint::crashIfHit("ckpt.before_wal_reset");
    wal_.reset();
  } catch (...) {
    failed_ = true;
    throw;
  }
}

void DB::maybeCheckpoint() {
  if (wal_.size() >= options_.checkpoint_wal_bytes || pager_.dirtyCount() >= options_.checkpoint_dirty_pages) {
    checkpoint();
  }
}

std::string DB::checkIntegrity() {
  ensureUsable();
  return tree_.check();
}

DBStats DB::stats() {
  ensureUsable();
  DBStats s;
  const PagerStats& ps = pager_.stats();
  s.keys = pager_.meta().key_count;
  s.height = tree_.height();
  s.pages = pager_.meta().page_count;
  s.dirty_pages = pager_.dirtyCount();
  s.cached_pages = pager_.cachedCount();
  s.wal_bytes = wal_.size();
  s.last_lsn = last_lsn_;
  s.checkpoints = ps.checkpoints;
  s.cache_hits = ps.cache_hits;
  s.cache_misses = ps.cache_misses;
  s.wal_records_replayed = replayed_;
  s.wal_bytes_truncated = wal_.truncatedBytes();
  s.recovered_from_doublewrite = ps.recovered_from_doublewrite;
  return s;
}

void DB::destroy(const std::string& dir) {
  removeFile(joinPath(dir, kDataFile));
  removeFile(joinPath(dir, kWalFile));
  removeFile(joinPath(dir, kDoublewriteFile));
}

}  // namespace pkv
