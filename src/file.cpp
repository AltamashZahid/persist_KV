#include "persistkv/file.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "persistkv/common.h"

#ifdef _WIN32
#include <windows.h>
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
#ifdef _WIN32
  throw Error(std::string(what) + " failed for '" + path + "': Windows error " + std::to_string(::GetLastError()));
#else
  throw Error(std::string(what) + " failed for '" + path + "': " + std::strerror(errno));
#endif
}

#ifdef _WIN32
// Positional I/O on a synchronous handle: ReadFile/WriteFile with an offset
// in an OVERLAPPED block read or write there and block until done, without
// using (or racing on) the handle's file pointer.
bool positionalIO(HANDLE h, bool write, uint64_t at, void* buf, DWORD n, DWORD* done) {
  OVERLAPPED ov;
  std::memset(&ov, 0, sizeof ov);
  ov.Offset = static_cast<DWORD>(at & 0xFFFFFFFFu);
  ov.OffsetHigh = static_cast<DWORD>(at >> 32);
  *done = 0;
  BOOL ok = write ? ::WriteFile(h, buf, n, done, &ov) : ::ReadFile(h, buf, n, done, &ov);
  if (!ok && !write && ::GetLastError() == ERROR_HANDLE_EOF) {
    *done = 0;
    return true;
  }
  return ok != 0;
}

HANDLE openHandle(const std::string& path, DWORD access, DWORD disposition) {
  return ::CreateFileA(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disposition,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}
#endif

}  // namespace

File::~File() { close(); }

void File::open(const std::string& path) {
  close();
  path_ = path;
#ifdef _WIN32
  HANDLE h = openHandle(path, GENERIC_READ | GENERIC_WRITE, OPEN_ALWAYS);
  if (h == INVALID_HANDLE_VALUE) ioFail("open", path);
  handle_ = h;
#else
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd_ < 0) ioFail("open", path);
#endif
}

#ifdef _WIN32
// Windows serializes synchronous I/O on one file handle, so concurrent
// readers each borrow a read-only handle of their own (opened on first
// need, kept for reuse); reads through different handles run in parallel.
void* File::takeReader() {
  {
    LockGuard lock(readers_mu_);
    if (!readers_.empty()) {
      void* r = readers_.back();
      readers_.pop_back();
      return r;
    }
  }
  HANDLE r = openHandle(path_, GENERIC_READ, OPEN_EXISTING);
  if (r == INVALID_HANDLE_VALUE) ioFail("open", path_);
  return r;
}

void File::giveReader(void* r) {
  LockGuard lock(readers_mu_);
  readers_.push_back(r);
}
#endif

void File::close() {
#ifdef _WIN32
  if (!handle_) return;
  ::CloseHandle(static_cast<HANDLE>(handle_));
  handle_ = nullptr;
  for (void* r : readers_) ::CloseHandle(static_cast<HANDLE>(r));
  readers_.clear();
#else
  if (fd_ < 0) return;
  ::close(fd_);
  fd_ = -1;
#endif
}

size_t File::readAt(uint64_t offset, void* buf, size_t n) {
  char* p = static_cast<char*>(buf);
  size_t done = 0;
#ifdef _WIN32
  while (done < n) {
    DWORD got = 0;
    void* r = takeReader();
    bool ok = positionalIO(static_cast<HANDLE>(r), false, offset + done, p + done, static_cast<DWORD>(n - done), &got);
    giveReader(r);
    if (!ok) ioFail("read", path_);
    if (got == 0) break;  // end of file
    done += got;
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
  while (done < n) {
    DWORD put = 0;
    bool ok = positionalIO(static_cast<HANDLE>(handle_), true, offset + done, const_cast<char*>(p + done),
                           static_cast<DWORD>(n - done), &put);
    if (!ok || put == 0) ioFail("write", path_);
    done += put;
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
  if (!::FlushFileBuffers(static_cast<HANDLE>(handle_))) ioFail("sync", path_);
#else
  if (::fsync(fd_) != 0) ioFail("sync", path_);
#endif
}

void File::truncate(uint64_t size) {
#ifdef _WIN32
  LARGE_INTEGER at;
  at.QuadPart = static_cast<LONGLONG>(size);
  HANDLE h = static_cast<HANDLE>(handle_);
  if (!::SetFilePointerEx(h, at, nullptr, FILE_BEGIN) || !::SetEndOfFile(h)) ioFail("truncate", path_);
#else
  if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) ioFail("truncate", path_);
#endif
}

uint64_t File::size() {
#ifdef _WIN32
  LARGE_INTEGER n;
  if (!::GetFileSizeEx(static_cast<HANDLE>(handle_), &n)) ioFail("stat", path_);
  return static_cast<uint64_t>(n.QuadPart);
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
