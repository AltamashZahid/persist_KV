#include "persistkv/crc32.h"

namespace pkv {

namespace {

struct CrcTable {
  uint32_t t[256];
  CrcTable() {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
  }
};

const CrcTable& table() {
  static const CrcTable tbl;
  return tbl;
}

}  // namespace

uint32_t crc32(const void* data, size_t n, uint32_t crc) {
  const uint32_t* t = table().t;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  crc = ~crc;
  while (n--) crc = t[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

}  // namespace pkv
