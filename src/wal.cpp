#include "persistkv/wal.h"

#include <vector>

#include "persistkv/crc32.h"
#include "persistkv/failpoint.h"

namespace pkv {

namespace {

constexpr size_t kRecordHeaderSize = 8;  // crc + len
constexpr size_t kBatchHeaderSize = 12;  // lsn + count
constexpr size_t kEntryHeaderSize = 7;   // op + klen + vlen

// Parses one batch payload; returns false if it is malformed.
bool parseBatch(const char* p, size_t len, Lsn* lsn, WriteBatch* batch) {
  if (len < kBatchHeaderSize) return false;
  *lsn = get64(p);
  uint32_t count = get32(p + 8);
  size_t off = kBatchHeaderSize;
  for (uint32_t i = 0; i < count; i++) {
    if (off + kEntryHeaderSize > len) return false;
    uint8_t op = static_cast<uint8_t>(p[off]);
    uint16_t klen = get16(p + off + 1);
    uint32_t vlen = get32(p + off + 3);
    off += kEntryHeaderSize;
    if (klen > kMaxKeySize || vlen > kMaxValueSize || off + klen + vlen > len) return false;
    std::string key(p + off, klen);
    if (op == static_cast<uint8_t>(WalOp::Put)) {
      batch->put(key, std::string(p + off + klen, vlen));
    } else if (op == static_cast<uint8_t>(WalOp::Delete) && vlen == 0) {
      batch->remove(key);
    } else {
      return false;
    }
    off += klen + vlen;
  }
  return off == len;
}

}  // namespace

void Wal::open() {
  file_.open(path_);
  size_ = file_.size();
}

Lsn Wal::replay(const ReplayFn& fn) {
  uint64_t file_size = file_.size();
  std::vector<char> buf(file_size);
  file_size = file_.readAt(0, buf.data(), buf.size());

  size_t off = 0;
  Lsn last = 0;
  while (off + kRecordHeaderSize <= file_size) {
    const char* rec = buf.data() + off;
    uint32_t len = get32(rec + 4);
    if (len < kBatchHeaderSize || off + kRecordHeaderSize + len > file_size) break;
    if (crc32(rec + 4, 4 + static_cast<size_t>(len)) != get32(rec)) break;

    Lsn lsn;
    WriteBatch batch;
    if (!parseBatch(rec + kRecordHeaderSize, len, &lsn, &batch)) break;
    fn(lsn, batch);
    last = lsn;
    off += kRecordHeaderSize + len;
  }

  // Anything past the last good record is a torn tail from a crash mid-append.
  // It was never acknowledged, so dropping it loses nothing.
  if (off != file_size) {
    truncated_bytes_ = file_size - off;
    file_.truncate(off);
    file_.sync();
  }
  size_ = off;
  return last;
}

void Wal::append(Lsn lsn, const WriteBatch& batch, bool sync) {
  size_t len = kBatchHeaderSize + batch.count() * kEntryHeaderSize + batch.byteSize();
  std::vector<char> rec(kRecordHeaderSize + len);
  put32(&rec[4], static_cast<uint32_t>(len));
  char* p = &rec[kRecordHeaderSize];
  put64(p, lsn);
  put32(p + 8, static_cast<uint32_t>(batch.count()));
  size_t off = kBatchHeaderSize;
  for (const WriteBatch::Entry& e : batch.entries()) {
    p[off] = static_cast<char>(e.op);
    put16(p + off + 1, static_cast<uint16_t>(e.key.size()));
    put32(p + off + 3, static_cast<uint32_t>(e.value.size()));
    off += kEntryHeaderSize;
    std::memcpy(p + off, e.key.data(), e.key.size());
    off += e.key.size();
    if (!e.value.empty()) std::memcpy(p + off, e.value.data(), e.value.size());
    off += e.value.size();
  }
  put32(&rec[0], crc32(&rec[4], rec.size() - 4));

  if (failpoint::hit("wal.torn")) {
    file_.writeAt(size_, rec.data(), rec.size() / 2);
    failpoint::crash();
  }
  file_.writeAt(size_, rec.data(), rec.size());
  size_ += rec.size();
  if (sync) file_.sync();
}

void Wal::reset() {
  file_.truncate(0);
  file_.sync();
  size_ = 0;
}

}  // namespace pkv
