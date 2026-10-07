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

  // 2. Redo every logged operation the data file has not seen yet.
  wal_.open();
  Lsn checkpointed = pager_.meta().checkpoint_lsn;
  last_lsn_ = checkpointed;
  Lsn wal_last = wal_.replay([&](const WalRecord& rec) {
    if (rec.lsn <= checkpointed) return;  // already in the data file
    apply(rec);
    replayed_++;
  });
  last_lsn_ = std::max(last_lsn_, wal_last);

  // 3. Make the recovered state durable so the WAL can start empty.
  if (fresh || replayed_ > 0) checkpoint();
}

DB::~DB() {
  if (abandoned_) return;
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

void DB::apply(const WalRecord& rec) {
  if (rec.op == WalOp::Put) {
    tree_.put(rec.key, rec.value);
  } else {
    tree_.remove(rec.key);
  }
}

void DB::put(const std::string& key, const std::string& value) {
  validate(key, &value);
  WalRecord rec;
  rec.lsn = last_lsn_ + 1;
  rec.op = WalOp::Put;
  rec.key = key;
  rec.value = value;

  wal_.append(rec, options_.sync_writes);  // durable from here on
  last_lsn_ = rec.lsn;
  failpoint::crashIfHit("db.after_wal");
  apply(rec);
  maybeCheckpoint();
}

bool DB::get(const std::string& key, std::string* value) {
  validate(key, nullptr);
  return tree_.get(key, value);
}

bool DB::remove(const std::string& key) {
  validate(key, nullptr);
  if (!tree_.get(key, nullptr)) return false;  // nothing to log

  WalRecord rec;
  rec.lsn = last_lsn_ + 1;
  rec.op = WalOp::Delete;
  rec.key = key;

  wal_.append(rec, options_.sync_writes);
  last_lsn_ = rec.lsn;
  failpoint::crashIfHit("db.after_wal");
  apply(rec);
  maybeCheckpoint();
  return true;
}

void DB::scan(const std::string& lo, const std::string& hi, const ScanFn& fn) {
  tree_.scan(lo, hi.empty() ? nullptr : &hi, fn);
}

void DB::checkpoint() {
  pager_.checkpoint(last_lsn_);
  failpoint::crashIfHit("ckpt.before_wal_reset");
  wal_.reset();
}

void DB::maybeCheckpoint() {
  if (wal_.size() >= options_.checkpoint_wal_bytes || pager_.dirtyCount() >= options_.checkpoint_dirty_pages) {
    checkpoint();
  }
}

DBStats DB::stats() {
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
