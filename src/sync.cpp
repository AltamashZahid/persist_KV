#include "persistkv/sync.h"

#include "persistkv/common.h"

#ifdef _WIN32
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600  // SRW locks and condition variables need Vista+
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#endif

namespace pkv {

#ifdef _WIN32

static_assert(sizeof(SRWLOCK) == sizeof(void*), "SRWLOCK must fit in a pointer");
static_assert(sizeof(CONDITION_VARIABLE) == sizeof(void*), "CONDITION_VARIABLE must fit in a pointer");

namespace {
PSRWLOCK srw(void*& p) { return reinterpret_cast<PSRWLOCK>(&p); }
PCONDITION_VARIABLE cond(void*& p) { return reinterpret_cast<PCONDITION_VARIABLE>(&p); }

unsigned __stdcall threadEntry(void* arg) {
  std::function<void()>* fn = static_cast<std::function<void()>*>(arg);
  (*fn)();
  delete fn;
  return 0;
}
}  // namespace

void Mutex::lock() { AcquireSRWLockExclusive(srw(srw_)); }
void Mutex::unlock() { ReleaseSRWLockExclusive(srw(srw_)); }

void SharedMutex::lock() { AcquireSRWLockExclusive(srw(srw_)); }
void SharedMutex::unlock() { ReleaseSRWLockExclusive(srw(srw_)); }
void SharedMutex::lockShared() { AcquireSRWLockShared(srw(srw_)); }
void SharedMutex::unlockShared() { ReleaseSRWLockShared(srw(srw_)); }

void CondVar::wait(Mutex& mu) { SleepConditionVariableSRW(cond(cv_), srw(mu.srw_), INFINITE, 0); }
void CondVar::notifyOne() { WakeConditionVariable(cond(cv_)); }
void CondVar::notifyAll() { WakeAllConditionVariable(cond(cv_)); }

void Thread::start(std::function<void()> fn) {
  auto* heap_fn = new std::function<void()>(std::move(fn));
  uintptr_t h = _beginthreadex(nullptr, 0, threadEntry, heap_fn, 0, nullptr);
  if (h == 0) {
    delete heap_fn;
    throw Error("failed to start thread");
  }
  handle_ = reinterpret_cast<void*>(h);
}

void Thread::join() {
  if (!handle_) return;
  WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
  CloseHandle(static_cast<HANDLE>(handle_));
  handle_ = nullptr;
}

#else

void Mutex::lock() { m_.lock(); }
void Mutex::unlock() { m_.unlock(); }

void SharedMutex::lock() {
  std::unique_lock<std::mutex> lock(m_);
  waiting_writers_++;
  writers_cv_.wait(lock, [this] { return !writer_ && readers_ == 0; });
  waiting_writers_--;
  writer_ = true;
}

void SharedMutex::unlock() {
  std::lock_guard<std::mutex> lock(m_);
  writer_ = false;
  if (waiting_writers_ > 0) {
    writers_cv_.notify_one();
  } else {
    readers_cv_.notify_all();
  }
}

void SharedMutex::lockShared() {
  std::unique_lock<std::mutex> lock(m_);
  readers_cv_.wait(lock, [this] { return !writer_ && waiting_writers_ == 0; });
  readers_++;
}

void SharedMutex::unlockShared() {
  std::lock_guard<std::mutex> lock(m_);
  if (--readers_ == 0 && waiting_writers_ > 0) writers_cv_.notify_one();
}

void CondVar::wait(Mutex& mu) {
  std::unique_lock<std::mutex> lock(mu.m_, std::adopt_lock);
  cv_.wait(lock);
  lock.release();  // the caller still owns the mutex
}
void CondVar::notifyOne() { cv_.notify_one(); }
void CondVar::notifyAll() { cv_.notify_all(); }

void Thread::start(std::function<void()> fn) { t_ = std::thread(std::move(fn)); }
void Thread::join() {
  if (t_.joinable()) t_.join();
}

#endif

Thread::~Thread() { join(); }

}  // namespace pkv
