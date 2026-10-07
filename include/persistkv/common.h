#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace pkv {

using PageId = uint32_t;
using Lsn = uint64_t;

constexpr uint32_t kPageSize = 4096;
constexpr uint32_t kMaxKeySize = 128;
constexpr uint32_t kMaxValueSize = 1u << 20;  // 1 MiB; large values live in overflow pages

// Page 0 is always the meta page, so 0 can never be a tree node or free page.
constexpr PageId kInvalidPage = 0;

class Error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Thrown when on-disk bytes fail a checksum or structural check.
class CorruptionError : public Error {
 public:
  using Error::Error;
};

// Fixed-width little-endian encoding helpers (assumes a little-endian host,
// which covers x86 and ARM).
inline void put16(char* p, uint16_t v) { std::memcpy(p, &v, sizeof v); }
inline void put32(char* p, uint32_t v) { std::memcpy(p, &v, sizeof v); }
inline void put64(char* p, uint64_t v) { std::memcpy(p, &v, sizeof v); }
inline uint16_t get16(const char* p) { uint16_t v; std::memcpy(&v, p, sizeof v); return v; }
inline uint32_t get32(const char* p) { uint32_t v; std::memcpy(&v, p, sizeof v); return v; }
inline uint64_t get64(const char* p) { uint64_t v; std::memcpy(&v, p, sizeof v); return v; }

}  // namespace pkv
