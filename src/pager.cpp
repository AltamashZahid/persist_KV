#include "persistkv/pager.h"

#include <algorithm>

#include "persistkv/crc32.h"
#include "persistkv/failpoint.h"

namespace pkv {

namespace {

constexpr uint64_t kMetaMagic = 0x31304244564B5650ull;  // "PKVDB01"
constexpr uint32_t kFormatVersion = 3;  // 2: variable-size nodes, overflow pages; 3: slotted pages

// Meta page body layout (after the common header).
constexpr uint32_t kOffMagic = 16;
constexpr uint32_t kOffVersion = 24;
constexpr uint32_t kOffPageSize = 28;
constexpr uint32_t kOffRoot = 32;
constexpr uint32_t kOffPageCount = 36;
constexpr uint32_t kOffFreeHead = 40;
constexpr uint32_t kOffCheckpointLsn = 48;
constexpr uint32_t kOffKeyCount = 56;

// Doublewrite file: [magic u32][count u32][crc u32][reserved u32]
// followed by `count` entries of [page id u32][page bytes].
constexpr uint32_t kDwbMagic = 0x31425744;  // "DWB1"
constexpr uint32_t kDwbHeaderSize = 16;
constexpr uint32_t kDwbEntrySize = 4 + kPageSize;

}  // namespace

void stampChecksum(char* page) { put32(page, crc32(page + 4, kPageSize - 4)); }

bool verifyChecksum(const char* page) { return get32(page) == crc32(page + 4, kPageSize - 4); }

Pager::Pager(std::string data_path, std::string dwb_path, size_t cache_pages)
    : data_path_(std::move(data_path)),
      dwb_path_(std::move(dwb_path)),
      capacity_(std::max<size_t>(cache_pages, 16)) {
  // Up to 16 shards, each holding at least 64 pages (small caches keep one).
  size_t n = std::min<size_t>(16, std::max<size_t>(1, capacity_ / 64));
  for (size_t i = 0; i < n; i++) {
    shards_.emplace_back(new Shard());
    shards_.back()->capacity = (capacity_ + n - 1) / n;
  }
}

void Pager::open() {
  data_.open(data_path_);
  dwb_.open(dwb_path_);
  recoverDoublewrite();

  if (data_.size() == 0) {
    is_new_ = true;
    meta_ = Meta();
    return;
  }
  std::vector<char> page(kPageSize);
  if (data_.readAt(0, page.data(), kPageSize) != kPageSize) {
    throw CorruptionError("data file too short to hold the meta page");
  }
  if (!verifyChecksum(page.data())) throw CorruptionError("meta page checksum mismatch");
  decodeMeta(page.data());
}

void Pager::recoverDoublewrite() {
  uint64_t size = dwb_.size();
  if (size == 0) return;

  std::vector<char> buf(size);
  bool valid = dwb_.readAt(0, buf.data(), size) == size && size >= kDwbHeaderSize &&
               get32(&buf[0]) == kDwbMagic;
  if (valid) {
    uint64_t count = get32(&buf[4]);
    valid = size == kDwbHeaderSize + count * kDwbEntrySize &&
            get32(&buf[8]) == crc32(buf.data() + kDwbHeaderSize, size - kDwbHeaderSize);
  }
  if (valid) {
    // The checkpoint may have died mid-way through in-place writes. The
    // doublewrite copy is complete and verified, so re-apply all of it.
    uint32_t count = get32(&buf[4]);
    for (uint32_t i = 0; i < count; i++) {
      const char* entry = &buf[kDwbHeaderSize + static_cast<size_t>(i) * kDwbEntrySize];
      data_.writeAt(static_cast<uint64_t>(get32(entry)) * kPageSize, entry + 4, kPageSize);
    }
    data_.sync();
    stats_.recovered_from_doublewrite = true;
  }
  // An invalid (torn) doublewrite file means the crash happened before any
  // in-place write, so the data file is still the previous consistent state.
  dwb_.truncate(0);
  dwb_.sync();
}

void Pager::encodeMeta(char* page) const {
  std::memset(page, 0, kPageSize);
  page[kOffType] = static_cast<char>(PageType::Meta);
  put64(page + kOffMagic, kMetaMagic);
  put32(page + kOffVersion, kFormatVersion);
  put32(page + kOffPageSize, kPageSize);
  put32(page + kOffRoot, meta_.root);
  put32(page + kOffPageCount, meta_.page_count);
  put32(page + kOffFreeHead, meta_.free_head);
  put64(page + kOffCheckpointLsn, meta_.checkpoint_lsn);
  put64(page + kOffKeyCount, meta_.key_count);
  stampChecksum(page);
}

void Pager::decodeMeta(const char* page) {
  if (static_cast<PageType>(page[kOffType]) != PageType::Meta || get64(page + kOffMagic) != kMetaMagic) {
    throw CorruptionError("not a PersistKV data file (bad magic)");
  }
  if (get32(page + kOffVersion) != kFormatVersion) throw CorruptionError("unsupported format version");
  if (get32(page + kOffPageSize) != kPageSize) throw CorruptionError("page size mismatch");
  meta_.root = get32(page + kOffRoot);
  meta_.page_count = get32(page + kOffPageCount);
  meta_.free_head = get32(page + kOffFreeHead);
  meta_.checkpoint_lsn = get64(page + kOffCheckpointLsn);
  meta_.key_count = get64(page + kOffKeyCount);
}

void Pager::loadFromDisk(PageId id, char* out) {
  if (data_.readAt(static_cast<uint64_t>(id) * kPageSize, out, kPageSize) != kPageSize) {
    throw CorruptionError("short read on page " + std::to_string(id));
  }
  if (!verifyChecksum(out)) throw CorruptionError("checksum mismatch on page " + std::to_string(id));
}

Pager::Frame*& Pager::slot(Shard& sh, PageId id) {
  size_t i = id / shards_.size();
  if (i >= sh.table.size()) sh.table.resize(std::max<size_t>(i + 1, sh.table.size() * 2), nullptr);
  return sh.table[i];
}

Pager::Frame* Pager::cachedFrame(Shard& sh, PageId id) {
  size_t i = id / shards_.size();
  if (i >= sh.table.size() || !sh.table[i]) return nullptr;
  Frame& f = *sh.table[i];
  sh.hits++;
  if (!f.held && f.lru != sh.lru.begin()) sh.lru.splice(sh.lru.begin(), sh.lru, f.lru);
  return &f;
}

Pager::Frame& Pager::installLoaded(Shard& sh, PageId id, std::vector<char> data) {
  if (Frame* f = cachedFrame(sh, id)) return *f;
  sh.misses++;
  Frame f;
  f.data = std::move(data);
  sh.lru.push_front(id);
  f.lru = sh.lru.begin();
  Frame& ref = sh.frames.emplace(id, std::move(f)).first->second;
  slot(sh, id) = &ref;
  evictIfNeeded(sh, id);
  return ref;
}

Pager::Frame& Pager::frame(Shard& sh, PageId id, bool load_from_disk) {
  if (Frame* f = cachedFrame(sh, id)) return *f;
  std::vector<char> data(kPageSize, 0);
  if (load_from_disk) loadFromDisk(id, data.data());
  return installLoaded(sh, id, std::move(data));
}

void Pager::evictIfNeeded(Shard& sh, PageId keep) {
  // Drop clean pages from the cold end. Dirty pages cannot be evicted (the
  // data file may only change during a checkpoint), so the cache can
  // temporarily grow past capacity until the next one. They are not in the
  // LRU list at all, so this never walks past them: it used to, and with the
  // cache full of dirty pages after a bulk load every miss scanned them all.
  while (sh.frames.size() > sh.capacity && !sh.lru.empty()) {
    PageId id = sh.lru.back();
    if (id == keep) break;
    slot(sh, id) = nullptr;
    sh.frames.erase(id);
    sh.lru.pop_back();
  }
}

void Pager::hold(Shard& sh, Frame& f) {
  if (f.held) return;
  sh.held.splice(sh.held.begin(), sh.lru, f.lru);
  f.held = true;
}

void Pager::release(Shard& sh, Frame& f) {
  if (!f.held || f.dirty || f.pins > 0) return;
  sh.lru.splice(sh.lru.begin(), sh.held, f.lru);
  f.held = false;
}

void Pager::read(PageId id, char* out) {
  if (id == kInvalidPage || id >= meta_.page_count) {
    throw CorruptionError("read of out-of-range page " + std::to_string(id));
  }
  Shard& sh = shardOf(id);
  {
    LockGuard lock(sh.mu);
    if (Frame* f = cachedFrame(sh, id)) {
      std::memcpy(out, f->data.data(), kPageSize);
      return;
    }
  }
  std::vector<char> buf(kPageSize);
  loadFromDisk(id, buf.data());  // without the shard lock, as in visit()
  LockGuard lock(sh.mu);
  std::memcpy(out, installLoaded(sh, id, std::move(buf)).data.data(), kPageSize);
}

void Pager::write(PageId id, const char* in) {
  if (id == kInvalidPage || id >= meta_.page_count) {
    throw Error("write of out-of-range page " + std::to_string(id));
  }
  Shard& sh = shardOf(id);
  LockGuard lock(sh.mu);
  Frame& f = frame(sh, id, false);
  std::memcpy(f.data.data(), in, kPageSize);
  if (!f.dirty) {
    f.dirty = true;
    sh.dirty++;
    hold(sh, f);
  }
}

PageId Pager::allocate() {
  if (meta_.free_head == kInvalidPage) return meta_.page_count++;
  PageId id = meta_.free_head;
  std::vector<char> page(kPageSize);
  read(id, page.data());
  if (static_cast<PageType>(page[kOffType]) != PageType::Free) {
    throw CorruptionError("free list points at non-free page " + std::to_string(id));
  }
  meta_.free_head = get32(&page[kOffNext]);
  return id;
}

void Pager::free(PageId id) {
  std::vector<char> page(kPageSize, 0);
  page[kOffType] = static_cast<char>(PageType::Free);
  put32(&page[kOffNext], meta_.free_head);
  write(id, page.data());
  meta_.free_head = id;
}

size_t Pager::dirtyCount() {
  size_t n = 0;
  for (auto& sh : shards_) {
    LockGuard lock(sh->mu);
    n += sh->dirty;
  }
  return n;
}

size_t Pager::cachedCount() {
  size_t n = 0;
  for (auto& sh : shards_) {
    LockGuard lock(sh->mu);
    n += sh->frames.size();
  }
  return n;
}

PagerStats Pager::stats() {
  PagerStats s;
  {
    LockGuard lock(stats_mu_);
    s = stats_;
  }
  s.cache_hits = 0;
  s.cache_misses = 0;
  for (auto& sh : shards_) {
    LockGuard lock(sh->mu);
    s.cache_hits += sh->hits;
    s.cache_misses += sh->misses;
  }
  return s;
}

CheckpointSnapshot Pager::beginCheckpoint(Lsn lsn) {
  CheckpointSnapshot snap;
  snap.lsn = lsn;
  meta_.checkpoint_lsn = lsn;
  snap.meta_page.resize(kPageSize);
  encodeMeta(snap.meta_page.data());

  for (auto& sh : shards_) {
    LockGuard lock(sh->mu);
    for (auto& kv : sh->frames) {
      if (kv.second.dirty) snap.ids.push_back(kv.first);
    }
  }
  std::sort(snap.ids.begin(), snap.ids.end());
  snap.pages.reserve(snap.ids.size());
  for (PageId id : snap.ids) {
    Shard& sh = shardOf(id);
    LockGuard lock(sh.mu);
    Frame& f = sh.frames[id];
    stampChecksum(f.data.data());
    snap.pages.push_back(f.data);
    f.dirty = false;  // a later write re-dirties it for the next checkpoint
    f.pins++;
    sh.dirty--;
  }
  return snap;
}

void Pager::writeCheckpoint(const CheckpointSnapshot& snap) {
  // 1. Doublewrite buffer: a complete, checksummed copy of every page we
  //    are about to overwrite.
  uint32_t count = static_cast<uint32_t>(snap.ids.size() + 1);
  std::vector<char> dwb(kDwbHeaderSize + static_cast<size_t>(count) * kDwbEntrySize);
  size_t off = kDwbHeaderSize;
  auto append = [&](PageId id, const char* page) {
    put32(&dwb[off], id);
    std::memcpy(&dwb[off + 4], page, kPageSize);
    off += kDwbEntrySize;
  };
  append(0, snap.meta_page.data());
  for (size_t i = 0; i < snap.ids.size(); i++) append(snap.ids[i], snap.pages[i].data());
  put32(&dwb[0], kDwbMagic);
  put32(&dwb[4], count);
  put32(&dwb[8], crc32(&dwb[kDwbHeaderSize], dwb.size() - kDwbHeaderSize));
  dwb_.writeAt(0, dwb.data(), dwb.size());
  dwb_.sync();
  failpoint::crashIfHit("ckpt.after_dwb");

  // 2. In-place writes. A crash here can leave torn pages behind; the
  //    doublewrite copy above repairs them on the next open.
  auto writeInPlace = [&](PageId id, const char* page) {
    uint64_t pos = static_cast<uint64_t>(id) * kPageSize;
    if (failpoint::hit("ckpt.torn_page")) {
      data_.writeAt(pos, page, kPageSize / 2);
      failpoint::crash();
    }
    data_.writeAt(pos, page, kPageSize);
  };
  writeInPlace(0, snap.meta_page.data());
  for (size_t i = 0; i < snap.ids.size(); i++) writeInPlace(snap.ids[i], snap.pages[i].data());
  data_.sync();
  failpoint::crashIfHit("ckpt.after_data");

  // 3. The data file is now consistent on its own; retire the doublewrite copy.
  dwb_.truncate(0);
  dwb_.sync();

  LockGuard lock(stats_mu_);
  stats_.pages_flushed += count;
  stats_.checkpoints++;
  is_new_ = false;
}

void Pager::endCheckpoint(const CheckpointSnapshot& snap) {
  for (PageId id : snap.ids) {
    Shard& sh = shardOf(id);
    LockGuard lock(sh.mu);
    auto it = sh.frames.find(id);
    if (it != sh.frames.end() && it->second.pins > 0) {
      it->second.pins--;
      release(sh, it->second);  // written out: evictable again unless re-dirtied
    }
  }
  for (auto& sh : shards_) {
    LockGuard lock(sh->mu);
    evictIfNeeded(*sh, kInvalidPage);
  }
}

void Pager::checkpoint(Lsn lsn) {
  CheckpointSnapshot snap = beginCheckpoint(lsn);
  writeCheckpoint(snap);
  endCheckpoint(snap);
}

}  // namespace pkv
