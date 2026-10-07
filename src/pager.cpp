#include "persistkv/pager.h"

#include <algorithm>

#include "persistkv/crc32.h"
#include "persistkv/failpoint.h"

namespace pkv {

namespace {

constexpr uint64_t kMetaMagic = 0x31304244564B5650ull;  // "PKVDB01"
constexpr uint32_t kFormatVersion = 2;  // 2: variable-size nodes, overflow pages

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
      capacity_(std::max<size_t>(cache_pages, 16)) {}

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

Pager::Frame& Pager::frame(PageId id, bool load_from_disk) {
  auto it = frames_.find(id);
  if (it != frames_.end()) {
    stats_.cache_hits++;
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    return it->second;
  }
  Frame f;
  f.data.assign(kPageSize, 0);
  if (load_from_disk) {
    stats_.cache_misses++;
    loadFromDisk(id, f.data.data());
  }
  lru_.push_front(id);
  f.lru = lru_.begin();
  Frame& ref = frames_.emplace(id, std::move(f)).first->second;
  evictIfNeeded(id);
  return ref;
}

void Pager::evictIfNeeded(PageId keep) {
  // Walk from the cold end and drop clean pages. Dirty pages cannot be
  // evicted (the data file may only change during a checkpoint), so the
  // cache can temporarily grow past capacity until the next checkpoint.
  auto it = lru_.end();
  while (frames_.size() > capacity_ && it != lru_.begin()) {
    --it;
    if (*it == keep) continue;
    auto f = frames_.find(*it);
    if (f->second.dirty) continue;
    frames_.erase(f);
    it = lru_.erase(it);
  }
}

void Pager::read(PageId id, char* out) {
  if (id == kInvalidPage || id >= meta_.page_count) {
    throw CorruptionError("read of out-of-range page " + std::to_string(id));
  }
  std::memcpy(out, frame(id, true).data.data(), kPageSize);
}

void Pager::write(PageId id, const char* in) {
  if (id == kInvalidPage || id >= meta_.page_count) {
    throw Error("write of out-of-range page " + std::to_string(id));
  }
  Frame& f = frame(id, false);
  std::memcpy(f.data.data(), in, kPageSize);
  if (!f.dirty) {
    f.dirty = true;
    dirty_count_++;
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

void Pager::checkpoint(Lsn lsn) {
  meta_.checkpoint_lsn = lsn;
  std::vector<char> meta_page(kPageSize);
  encodeMeta(meta_page.data());

  std::vector<PageId> dirty;
  for (auto& kv : frames_) {
    if (kv.second.dirty) {
      stampChecksum(kv.second.data.data());
      dirty.push_back(kv.first);
    }
  }
  std::sort(dirty.begin(), dirty.end());

  // 1. Doublewrite buffer: a complete, checksummed copy of every page we
  //    are about to overwrite.
  uint32_t count = static_cast<uint32_t>(dirty.size() + 1);
  std::vector<char> dwb(kDwbHeaderSize + static_cast<size_t>(count) * kDwbEntrySize);
  size_t off = kDwbHeaderSize;
  auto append = [&](PageId id, const char* page) {
    put32(&dwb[off], id);
    std::memcpy(&dwb[off + 4], page, kPageSize);
    off += kDwbEntrySize;
  };
  append(0, meta_page.data());
  for (PageId id : dirty) append(id, frames_[id].data.data());
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
  writeInPlace(0, meta_page.data());
  for (PageId id : dirty) writeInPlace(id, frames_[id].data.data());
  data_.sync();
  failpoint::crashIfHit("ckpt.after_data");

  // 3. The data file is now consistent on its own; retire the doublewrite copy.
  dwb_.truncate(0);
  dwb_.sync();

  for (PageId id : dirty) frames_[id].dirty = false;
  dirty_count_ = 0;
  stats_.pages_flushed += count;
  stats_.checkpoints++;
  is_new_ = false;
  evictIfNeeded(kInvalidPage);
}

}  // namespace pkv
