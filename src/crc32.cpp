#include "persistkv/crc32.h"

namespace pkv {

namespace {

// Slicing-by-8: t[k][b] is the CRC of byte b followed by k zero bytes, so
// eight input bytes are folded in with eight table lookups and no
// per-byte dependency chain.
struct CrcTables {
  uint32_t t[8][256];
  CrcTables() {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++) {
      for (int k = 1; k < 8; k++) t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xFF];
    }
  }
};

const CrcTables& tables() {
  static const CrcTables tbl;
  return tbl;
}

}  // namespace

uint32_t crc32(const void* data, size_t n, uint32_t crc) {
  const CrcTables& tb = tables();
  const uint8_t* p = static_cast<const uint8_t*>(data);
  crc = ~crc;
  while (n >= 8) {
    // Bytes are combined explicitly, so this is correct on any endianness.
    uint32_t lo = crc ^ (static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
                         static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24);
    crc = tb.t[7][lo & 0xFF] ^ tb.t[6][(lo >> 8) & 0xFF] ^ tb.t[5][(lo >> 16) & 0xFF] ^ tb.t[4][lo >> 24] ^
          tb.t[3][p[4]] ^ tb.t[2][p[5]] ^ tb.t[1][p[6]] ^ tb.t[0][p[7]];
    p += 8;
    n -= 8;
  }
  while (n--) crc = tb.t[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

}  // namespace pkv
