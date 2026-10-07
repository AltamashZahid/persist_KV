#include "persistkv/wal.h"

#include <vector>

#include "persistkv/crc32.h"
#include "persistkv/failpoint.h"

namespace pkv {

namespace {

constexpr size_t kRecordHeaderSize = 8;  // crc + len
constexpr size_t kPayloadFixedSize = 13; // lsn + op + klen + vlen
constexpr size_t kMaxPayloadSize = kPayloadFixedSize + kMaxKeySize + kMaxValueSize;

}  // namespace

void Wal::open() {
  file_.open(path_);
  size_ = file_.size();
}

Lsn Wal::replay(const std::function<void(const WalRecord&)>& fn) {
  uint64_t file_size = file_.size();
  std::vector<char> buf(file_size);
  file_size = file_.readAt(0, buf.data(), buf.size());

  size_t off = 0;
  Lsn last = 0;
  while (off + kRecordHeaderSize <= file_size) {
    const char* rec = &buf[off];
    uint32_t len = get32(rec + 4);
    if (len < kPayloadFixedSize || len > kMaxPayloadSize || off + kRecordHeaderSize + len > file_size) break;
    if (crc32(rec + 4, 4 + len) != get32(rec)) break;

    const char* p = rec + kRecordHeaderSize;
    WalRecord r;
    r.lsn = get64(p);
    uint8_t op = static_cast<uint8_t>(p[8]);
    uint16_t klen = get16(p + 9);
    uint16_t vlen = get16(p + 11);
    if ((op != 1 && op != 2) || kPayloadFixedSize + klen + vlen != len) break;
    r.op = static_cast<WalOp>(op);
    r.key.assign(p + kPayloadFixedSize, klen);
    r.value.assign(p + kPayloadFixedSize + klen, vlen);

    fn(r);
    last = r.lsn;
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

void Wal::append(const WalRecord& r, bool sync) {
  size_t len = kPayloadFixedSize + r.key.size() + r.value.size();
  std::vector<char> rec(kRecordHeaderSize + len);
  put32(&rec[4], static_cast<uint32_t>(len));
  char* p = &rec[kRecordHeaderSize];
  put64(p, r.lsn);
  p[8] = static_cast<char>(r.op);
  put16(p + 9, static_cast<uint16_t>(r.key.size()));
  put16(p + 11, static_cast<uint16_t>(r.value.size()));
  std::memcpy(p + kPayloadFixedSize, r.key.data(), r.key.size());
  std::memcpy(p + kPayloadFixedSize + r.key.size(), r.value.data(), r.value.size());
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
