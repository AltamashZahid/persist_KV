#pragma once

#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "persistkv/common.h"
#include "persistkv/file.h"
#include "persistkv/sync.h"

namespace pkv {

enum class PageType : uint8_t { Meta = 1, Leaf = 2, Internal = 3, Free = 4, Overflow = 5 };

// Every page starts with a 16-byte header:
//   [0, 4)   CRC32 of bytes [4, kPageSize)
//   [4]      PageType
//   [6, 8)   number of keys (tree pages) / bytes used (overflow pages)
//   [8, 12)  next page id (leaf right-sibling, overflow chain, or free-list link)
constexpr uint32_t kPageHeaderSize = 16;
constexpr uint32_t kOffType = 4;
constexpr uint32_t kOffNumKeys = 6;
constexpr uint32_t kOffNext = 8;

void stampChecksum(char* page);
bool verifyChecksum(const char* page);

// In-memory copy of page 0.
struct Meta {
  PageId root = kInvalidPage;
  uint32_t page_count = 1;  // includes the meta page
  PageId free_head = kInvalidPage;
  Lsn checkpoint_lsn = 0;  // every WAL record <= this is reflected in the data file
  uint64_t key_count = 0;
};

struct PagerStats {
  uint64_t cache_hits = 0;
  uint64_t cache_misses = 0;
  uint64_t pages_flushed = 0;
  uint64_t checkpoints = 0;
  bool recovered_from_doublewrite = false;
};

// A consistent copy of every dirty page plus the meta page, taken at one
// instant. Writing it out can then proceed while the tree keeps changing.
struct CheckpointSnapshot {
  Lsn lsn = 0;
  std::vector<char> meta_page;
  std::vector<PageId> ids;          // sorted
  std::vector<std::vector<char>> pages;  // pages[i] is the content of ids[i]
};

// Owns the data file and the doublewrite buffer. Caches pages with LRU
// eviction; dirty pages stay in memory until the next checkpoint, so the data
// file only ever changes inside a checkpoint.
//
// Thread safety: the page cache is split into shards by page id, each with
// its own mutex, LRU lists and frame table (lock striping, like PostgreSQL's
// buffer-mapping partitions), so read() and visit() from many threads at
// once rarely contend. Calls that change the tree
// (write, allocate, free, meta()) must be serialized by the caller; the DB
// does so with its tree lock.
class Pager {
 public:
  Pager(std::string data_path, std::string dwb_path, size_t cache_pages);

  // Repairs a half-finished checkpoint from the doublewrite buffer, then
  // loads the meta page (or starts a fresh database if the file is empty).
  void open();
  bool isNew() const { return is_new_; }

  void read(PageId id, char* out);

  // Calls fn(page) with the cached page bytes, without copying the page.
  // The page's shard lock is held during fn, so fn must be short and must
  // not call back into the pager.
  template <typename Fn>
  void visit(PageId id, Fn&& fn) {
    if (id == kInvalidPage || id >= meta_.page_count) {
      throw CorruptionError("read of out-of-range page " + std::to_string(id));
    }
    Shard& sh = shardOf(id);
    {
      LockGuard lock(sh.mu);
      if (Frame* f = cachedFrame(sh, id)) {
        fn(static_cast<const char*>(f->data.data()));
        return;
      }
    }
    // A miss reads (and checksums) the page without the shard's lock, so
    // misses from several threads overlap; then the first copy cached wins.
    std::vector<char> buf(kPageSize);
    loadFromDisk(id, buf.data());
    LockGuard lock(sh.mu);
    fn(static_cast<const char*>(installLoaded(sh, id, std::move(buf)).data.data()));
  }
  void write(PageId id, const char* in);

  PageId allocate();  // caller must write() the page before reading it
  void free(PageId id);

  Meta& meta() { return meta_; }
  const Meta& meta() const { return meta_; }
  size_t dirtyCount();
  size_t cachedCount();
  PagerStats stats();

  // A checkpoint makes the data file reflect everything up to `lsn`:
  //   1. write all dirty pages + meta to the doublewrite file, fsync
  //   2. write them in place in the data file, fsync
  //   3. truncate the doublewrite file
  // A crash during (1) leaves the data file untouched; a crash during (2)
  // is repaired on the next open() by replaying the doublewrite file.
  //
  // It runs in three calls so the slow part needs no tree lock:
  //   beginCheckpoint  (tree must not be changing) copies the dirty pages,
  //                    marks them clean, and pins them in the cache;
  //   writeCheckpoint  (any time) performs steps 1-3;
  //   endCheckpoint    unpins the pages.
  // Pinning matters: until the data file holds a flushed page, evicting it
  // and reading it back from disk would return stale bytes.
  CheckpointSnapshot beginCheckpoint(Lsn lsn);
  void writeCheckpoint(const CheckpointSnapshot& snap);
  void endCheckpoint(const CheckpointSnapshot& snap);
  void checkpoint(Lsn lsn);  // all three, for single-threaded callers

 private:
  struct Frame {
    std::vector<char> data;
    bool dirty = false;
    int pins = 0;  // > 0 while a checkpoint is writing this page out
    bool held = false;  // in the shard's held list (dirty or pinned) rather than its LRU list
    std::list<PageId>::iterator lru;
  };

  // One stripe of the cache. Clean, unpinned pages are in `lru` (front =
  // most recently used): every page there can be evicted. Dirty and pinned
  // pages wait in `held` until a checkpoint has written them, so eviction
  // never walks past pages it cannot drop.
  struct Shard {
    Mutex mu;  // guards everything below
    std::unordered_map<PageId, Frame> frames;
    // Page ids are dense, so a shard's frames are found by indexing this
    // array with id / shard count instead of hashing; nullptr = not cached.
    std::vector<Frame*> table;
    std::list<PageId> lru;
    std::list<PageId> held;
    size_t capacity = 0;
    size_t dirty = 0;
    uint64_t hits = 0, misses = 0;
  };
  Shard& shardOf(PageId id) { return *shards_[id % shards_.size()]; }
  Frame*& slot(Shard& sh, PageId id);  // sh.table entry for id, growing the table

  void hold(Shard& sh, Frame& f);     // dirty or pinned: out of the eviction list
  void release(Shard& sh, Frame& f);  // clean and unpinned again: back into it

  // All with the shard's lock held:
  Frame& frame(Shard& sh, PageId id, bool load_from_disk);
  Frame* cachedFrame(Shard& sh, PageId id);  // a hit (moved to the LRU front), or null
  // Caches a page read from disk, unless another thread cached it first
  // (then that copy is returned).
  Frame& installLoaded(Shard& sh, PageId id, std::vector<char> data);
  void evictIfNeeded(Shard& sh, PageId keep);
  void loadFromDisk(PageId id, char* out);
  void recoverDoublewrite();
  void encodeMeta(char* page) const;
  void decodeMeta(const char* page);

  File data_;
  File dwb_;
  std::string data_path_;
  std::string dwb_path_;
  size_t capacity_;
  bool is_new_ = false;
  Meta meta_;

  std::vector<std::unique_ptr<Shard>> shards_;
  Mutex stats_mu_;  // guards stats_ (checkpoint counters) and is_new_ updates
  PagerStats stats_;
};

}  // namespace pkv
