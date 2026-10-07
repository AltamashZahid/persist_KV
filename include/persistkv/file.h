#pragma once

#include <cstdint>
#include <string>

#include "persistkv/sync.h"

namespace pkv {

// Thin positional-I/O wrapper over a raw file descriptor. No user-space
// buffering: every writeAt goes straight to the OS, and sync() forces it to
// stable storage (fsync on POSIX, _commit on Windows). Safe to use from
// several threads: POSIX pread/pwrite are atomic, and on Windows the
// seek + read/write pair is done under a lock.
class File {
 public:
  File() = default;
  ~File();
  File(const File&) = delete;
  File& operator=(const File&) = delete;

  void open(const std::string& path);  // creates the file if missing
  void close();

  // Returns the number of bytes read; short only at end of file.
  size_t readAt(uint64_t offset, void* buf, size_t n);
  void writeAt(uint64_t offset, const void* buf, size_t n);
  void sync();
  void truncate(uint64_t size);
  uint64_t size();

  const std::string& path() const { return path_; }

 private:
  int fd_ = -1;
  std::string path_;
#ifdef _WIN32
  Mutex io_mu_;  // seek and read/write are separate calls on Windows
#endif
};

void makeDir(const std::string& path);  // no error if it already exists
bool removeFile(const std::string& path);

}  // namespace pkv
