#pragma once

#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include "persistkv/common.h"
#include "persistkv/file.h"

namespace pkv {

enum class PageType : uint8_t { Meta = 1, Leaf = 2, Internal = 3, Free = 4 };

// Every page starts with a 16-byte header:
//   [0, 4)   CRC32 of bytes [4, kPageSize)
//   [4]      PageType
//   [6, 8)   number of keys (tree pages)
//   [8, 12)  next page id (leaf right-sibling, or free-list link)
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

// Owns the data file and the doublewrite buffer. Caches pages with LRU
// eviction; dirty pages stay pinned in memory until the next checkpoint, so
// the data file only ever changes inside checkpoint().
class Pager {
 public:
  Pager(std::string data_path, std::string dwb_path, size_t cache_pages);

  // Repairs a half-finished checkpoint from the doublewrite buffer, then
  // loads the meta page (or starts a fresh database if the file is empty).
  void open();
  bool isNew() const { return is_new_; }

  void read(PageId id, char* out);
  void write(PageId id, const char* in);

  PageId allocate();  // caller must write() the page before reading it
  void free(PageId id);

  Meta& meta() { return meta_; }
  const Meta& meta() const { return meta_; }
  size_t dirtyCount() const { return dirty_count_; }
  size_t cachedCount() const { return frames_.size(); }
  const PagerStats& stats() const { return stats_; }

  // Atomically makes the data file reflect everything up to `lsn`:
  //   1. write all dirty pages + meta to the doublewrite file, fsync
  //   2. write them in place in the data file, fsync
  //   3. truncate the doublewrite file
  // A crash during (1) leaves the data file untouched; a crash during (2)
  // is repaired on the next open() by replaying the doublewrite file.
  void checkpoint(Lsn lsn);

 private:
  struct Frame {
    std::vector<char> data;
    bool dirty = false;
    std::list<PageId>::iterator lru;
  };

  Frame& frame(PageId id, bool load_from_disk);
  void loadFromDisk(PageId id, char* out);
  void evictIfNeeded(PageId keep);
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
  std::unordered_map<PageId, Frame> frames_;
  std::list<PageId> lru_;  // front = most recently used
  size_t dirty_count_ = 0;
  PagerStats stats_;
};

}  // namespace pkv
