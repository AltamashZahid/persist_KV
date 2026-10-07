#pragma once

#include <cstddef>
#include <cstdint>

namespace pkv {

// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320).
// Chainable: crc32(b, nb, crc32(a, na)) == crc32(a||b).
uint32_t crc32(const void* data, size_t n, uint32_t crc = 0);

}  // namespace pkv
