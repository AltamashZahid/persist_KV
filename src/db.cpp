#include "persistkv/db.h"

#include <algorithm>
#include <exception>

#include "persistkv/failpoint.h"
#include "persistkv/file.h"

namespace pkv {

namespace {

std::string joinPath(const std::string& dir, const char* name) { return dir + "/" + name; }

// A leader stops adding batches to its group past this size.
constexpr size_t kMaxGroupBytes = 4u << 20;

}  // namespace

constexpr const char* DB::kDataFile;
constexpr const char* DB::kWalFile;
constexpr const char* DB::kWalFileAlt;
constexpr const char* DB::kDoublewriteFile;

// One thread waiting in the group commit queue.
struct DB::Writer {
  explicit Writer(const WriteBatch* b) : batch(b) {}
  const WriteBatch* batch;
  bool done = false;
  std::exception_ptr error;
};

DB::DB(const std::string& dir, const Options& options)
    : dir_(dir),
      options_(options),
      pager_(joinPath(dir, kDataFile), joinPath(dir, kDoublewriteFile), options.cache_pages),
      tree_(pager_),
      failed_(false) {
  makeDir(dir_);

  // 1. Finish or discard any interrupted checkpoint, then load the meta page.
  pager_.open();
  bool fresh = pager_.isNew();
  if (fresh) tree_.init();

  // 2. Collect the records from both WAL files and redo, in LSN order, every
  //    batch the data file has not seen yet. Two files exist because a
  //    checkpoint switches new appends to the other file while it runs.
  wals_[0].reset(new Wal(joinPath(dir, kWalFile)));
  wals_[1].reset(new Wal(joinPath(dir, kWalFileAlt)));
  struct Record {
    Lsn lsn;
    WriteBatch batch;
  };
  std::vector<Record> records;
  bool had_log = false;
  for (auto& wal : wals_) {
    wal->open();
    wal->replay([&](Lsn lsn, const WriteBatch& batch) { records.push_back({lsn, batch}); });
    truncated_ += wal->truncatedBytes();
    had_log = had_log || wal->size() > 0;
  }
  std::stable_sort(records.begin(), records.end(),
                   [](const Record& a, const Record& b) { return a.lsn < b.lsn; });

  Lsn checkpointed = pager_.meta().checkpoint_lsn;
  last_lsn_ = checkpointed;
  for (const Record& r : records) {
    last_lsn_ = std::max(last_lsn_, r.lsn);
    if (r.lsn <= checkpointed) continue;  // already in the data file
    applyBatch(r.batch);
    replayed_++;
  }

  // 3. Make the recovered state durable, then start both logs empty.
  if (fresh || replayed_ > 0 || had_log) {
    runCheckpoint();
    wals_[0]->reset();
    wals_[1]->reset();
    active_wal_ = 0;
  }

  if (options_.background_checkpoints) ckpt_thread_.start([this] { checkpointLoop(); });
}

DB::~DB() {
  {
    LockGuard lock(ckpt_mu_);
    stopping_ = true;
    ckpt_cv_.notifyAll();
  }
  ckpt_thread_.join();
  if (abandoned_ || failed_) return;  // never checkpoint a possibly half-applied tree
  try {
    runCheckpoint();
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

// ---------------------------------------------------------------------------
// Group commit
// ---------------------------------------------------------------------------

void DB::commit(const WriteBatch& batch) {
  ensureUsable();
  Writer w(&batch);

  commit_mu_.lock();
  writers_.push_back(&w);
  while (!w.done && writers_.front() != &w) commit_cv_.wait(commit_mu_);
  if (w.done) {
    // A leader already committed our batch together with its own.
    commit_mu_.unlock();
    if (w.error) std::rethrow_exception(w.error);
    return;
  }

  // We are the leader: take every queued batch (up to a size limit).
  std::vector<Writer*> group;
  size_t bytes = 0;
  for (Writer* x : writers_) {
    if (!group.empty() && bytes + x->batch->byteSize() > kMaxGroupBytes) break;
    group.push_back(x);
    bytes += x->batch->byteSize();
  }
  commit_mu_.unlock();

  bool want_checkpoint = false, must_wait = false;
  std::exception_ptr error;
  try {
    writeGroup(group, &want_checkpoint, &must_wait);
  } catch (...) {
    // The WAL or the tree may be half-updated; nothing more may be written.
    failed_ = true;
    error = std::current_exception();
  }

  commit_mu_.lock();
  for (size_t i = 0; i < group.size(); i++) {
    Writer* x = writers_.front();
    writers_.pop_front();
    x->error = error;
    x->done = true;
  }
  commit_cv_.notifyAll();  // wakes the followers and the next leader
  commit_mu_.unlock();

  if (error) std::rethrow_exception(error);
  if (want_checkpoint) requestCheckpoint(must_wait);
}

// Logs every batch in the group with a single fsync, then applies them.
void DB::writeGroup(const std::vector<Writer*>& group, bool* want_checkpoint, bool* must_wait) {
  LockGuard wal_lock(wal_mu_);
  ensureUsable();
  Wal& wal = *wals_[active_wal_];
  for (Writer* x : group) wal.append(++last_lsn_, *x->batch, false);
  if (options_.sync_writes) wal.sync();  // durable from here on
  failpoint::crashIfHit("db.after_wal");
  commit_groups_++;
  commit_batches_ += group.size();

  {
    ExclusiveLock tree_lock(tree_mu_);
    for (Writer* x : group) applyBatch(*x->batch);
  }

  size_t dirty = pager_.dirtyCount();
  *want_checkpoint = wal.size() >= options_.checkpoint_wal_bytes || dirty >= options_.checkpoint_dirty_pages;
  *must_wait = wal.size() >= 4 * options_.checkpoint_wal_bytes || dirty >= 4 * options_.checkpoint_dirty_pages;
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
  {
    SharedLock lock(tree_mu_);
    if (!tree_.get(key, nullptr)) return false;  // nothing to log
  }
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

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------

bool DB::get(const std::string& key, std::string* value) {
  validate(key, nullptr);
  ensureUsable();
  SharedLock lock(tree_mu_);
  return tree_.get(key, value);
}

void DB::scan(const std::string& lo, const std::string& hi, const ScanFn& fn) {
  ensureUsable();
  SharedLock lock(tree_mu_);
  tree_.scan(lo, hi.empty() ? nullptr : &hi, fn);
}

uint64_t DB::size() {
  SharedLock lock(tree_mu_);
  return pager_.meta().key_count;
}

std::string DB::checkIntegrity() {
  ensureUsable();
  SharedLock lock(tree_mu_);
  return tree_.check();
}

DBStats DB::stats() {
  ensureUsable();
  LockGuard wal_lock(wal_mu_);
  SharedLock tree_lock(tree_mu_);
  DBStats s;
  PagerStats ps = pager_.stats();
  s.keys = pager_.meta().key_count;
  s.height = tree_.height();
  s.pages = pager_.meta().page_count;
  s.dirty_pages = pager_.dirtyCount();
  s.cached_pages = pager_.cachedCount();
  s.wal_bytes = wals_[0]->size() + wals_[1]->size();
  s.last_lsn = last_lsn_;
  s.checkpoints = ps.checkpoints;
  s.cache_hits = ps.cache_hits;
  s.cache_misses = ps.cache_misses;
  s.commit_groups = commit_groups_;
  s.commit_batches = commit_batches_;
  s.wal_records_replayed = replayed_;
  s.wal_bytes_truncated = truncated_;
  s.recovered_from_doublewrite = ps.recovered_from_doublewrite;
  return s;
}

// ---------------------------------------------------------------------------
// Checkpoints
// ---------------------------------------------------------------------------

void DB::checkpoint() {
  ensureUsable();
  runCheckpoint();
}

// Only the snapshot step holds the WAL and tree locks; the doublewrite and
// data file I/O run while readers and writers continue.
void DB::runCheckpoint() {
  LockGuard run_lock(ckpt_run_mu_);
  if (failed_) return;
  try {
    CheckpointSnapshot snap;
    int retired;
    {
      LockGuard wal_lock(wal_mu_);
      ExclusiveLock tree_lock(tree_mu_);
      if (!pager_.isNew() && pager_.dirtyCount() == 0 && wals_[active_wal_]->size() == 0) return;
      // Records in the retiring log must be durable before newer records
      // start landing in the other one, or a power cut could leave a gap.
      wals_[active_wal_]->sync();
      snap = pager_.beginCheckpoint(last_lsn_);
      retired = active_wal_;
      active_wal_ = 1 - active_wal_;  // the other log is empty: the previous checkpoint cleared it
    }
    pager_.writeCheckpoint(snap);
    failpoint::crashIfHit("ckpt.before_wal_reset");
    wals_[retired]->reset();  // everything in it is now in the data file
    pager_.endCheckpoint(snap);
  } catch (...) {
    failed_ = true;
    throw;
  }
}

void DB::requestCheckpoint(bool wait) {
  if (!options_.background_checkpoints) {
    runCheckpoint();
    return;
  }
  LockGuard lock(ckpt_mu_);
  ckpt_requested_ = true;
  ckpt_cv_.notifyAll();
  if (!wait) return;
  // Backpressure: the backlog is far past the threshold, so let the
  // checkpoint thread catch up before accepting more writes.
  uint64_t target = ckpt_done_ + 1;
  while (ckpt_done_ < target && !stopping_ && !failed_) ckpt_cv_.wait(ckpt_mu_);
}

void DB::checkpointLoop() {
  while (true) {
    {
      LockGuard lock(ckpt_mu_);
      while (!ckpt_requested_ && !stopping_) ckpt_cv_.wait(ckpt_mu_);
      if (!ckpt_requested_) return;  // stopping, nothing pending
      ckpt_requested_ = false;
    }
    try {
      runCheckpoint();
    } catch (...) {
      // runCheckpoint() marked the DB failed; callers will see it.
    }
    LockGuard lock(ckpt_mu_);
    ckpt_done_++;
    ckpt_cv_.notifyAll();
  }
}

void DB::destroy(const std::string& dir) {
  removeFile(joinPath(dir, kDataFile));
  removeFile(joinPath(dir, kWalFile));
  removeFile(joinPath(dir, kWalFileAlt));
  removeFile(joinPath(dir, kDoublewriteFile));
}

}  // namespace pkv
