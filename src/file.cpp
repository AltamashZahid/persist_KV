#include "persistkv/file.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "persistkv/common.h"

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace pkv {

namespace {

[[noreturn]] void ioFail(const char* what, const std::string& path) {
  throw Error(std::string(what) + " failed for '" + path + "': " + std::strerror(errno));
}

}  // namespace

File::~File() { close(); }

void File::open(const std::string& path) {
  close();
  path_ = path;
#ifdef _WIN32
  fd_ = ::_open(path.c_str(), _O_RDWR | _O_CREAT | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
#endif
  if (fd_ < 0) ioFail("open", path);
}

void File::close() {
  if (fd_ < 0) return;
#ifdef _WIN32
  ::_close(fd_);
#else
  ::close(fd_);
#endif
  fd_ = -1;
}

size_t File::readAt(uint64_t offset, void* buf, size_t n) {
  char* p = static_cast<char*>(buf);
  size_t done = 0;
#ifdef _WIN32
  LockGuard lock(io_mu_);
  if (::_lseeki64(fd_, static_cast<__int64>(offset), SEEK_SET) < 0) ioFail("seek", path_);
  while (done < n) {
    int r = ::_read(fd_, p + done, static_cast<unsigned>(n - done));
    if (r < 0) ioFail("read", path_);
    if (r == 0) break;
    done += static_cast<size_t>(r);
  }
#else
  while (done < n) {
    ssize_t r = ::pread(fd_, p + done, n - done, static_cast<off_t>(offset + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      ioFail("read", path_);
    }
    if (r == 0) break;
    done += static_cast<size_t>(r);
  }
#endif
  return done;
}

void File::writeAt(uint64_t offset, const void* buf, size_t n) {
  const char* p = static_cast<const char*>(buf);
  size_t done = 0;
#ifdef _WIN32
  LockGuard lock(io_mu_);
  if (::_lseeki64(fd_, static_cast<__int64>(offset), SEEK_SET) < 0) ioFail("seek", path_);
  while (done < n) {
    int r = ::_write(fd_, p + done, static_cast<unsigned>(n - done));
    if (r <= 0) ioFail("write", path_);
    done += static_cast<size_t>(r);
  }
#else
  while (done < n) {
    ssize_t r = ::pwrite(fd_, p + done, n - done, static_cast<off_t>(offset + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      ioFail("write", path_);
    }
    done += static_cast<size_t>(r);
  }
#endif
}

void File::sync() {
#ifdef _WIN32
  if (::_commit(fd_) != 0) ioFail("sync", path_);
#else
  if (::fsync(fd_) != 0) ioFail("sync", path_);
#endif
}

void File::truncate(uint64_t size) {
#ifdef _WIN32
  if (::_chsize(fd_, static_cast<long>(size)) != 0) ioFail("truncate", path_);
#else
  if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) ioFail("truncate", path_);
#endif
}

uint64_t File::size() {
#ifdef _WIN32
  LockGuard lock(io_mu_);
  __int64 end = ::_lseeki64(fd_, 0, SEEK_END);
  if (end < 0) ioFail("seek", path_);
  return static_cast<uint64_t>(end);
#else
  struct stat st;
  if (::fstat(fd_, &st) != 0) ioFail("stat", path_);
  return static_cast<uint64_t>(st.st_size);
#endif
}

void makeDir(const std::string& path) {
#ifdef _WIN32
  int r = ::_mkdir(path.c_str());
#else
  int r = ::mkdir(path.c_str(), 0755);
#endif
  if (r != 0 && errno != EEXIST) ioFail("mkdir", path);
}

bool removeFile(const std::string& path) { return std::remove(path.c_str()) == 0; }

}  // namespace pkv
