#pragma once

// Minimal threading primitives. On Windows they wrap SRW locks, condition
// variables and native threads directly (older MinGW toolchains ship without
// std::thread / std::mutex); elsewhere they wrap the C++ standard library.

#include <functional>

#ifndef _WIN32
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

namespace pkv {

class CondVar;

class Mutex {
 public:
  Mutex() = default;
  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;
  void lock();
  void unlock();

 private:
  friend class CondVar;
#ifdef _WIN32
  void* srw_ = nullptr;  // SRWLOCK, which is a single pointer initialized to 0
#else
  std::mutex m_;
#endif
};

// Reader-writer lock: many shared holders or one exclusive holder. Writers
// take priority: once one is waiting, new readers wait behind it, so a
// steady stream of readers cannot starve writers. (glibc's default rwlock,
// which std::shared_timed_mutex uses, prefers readers.)
class SharedMutex {
 public:
  SharedMutex() = default;
  SharedMutex(const SharedMutex&) = delete;
  SharedMutex& operator=(const SharedMutex&) = delete;
  void lock();
  void unlock();
  void lockShared();
  void unlockShared();

 private:
#ifdef _WIN32
  void* srw_ = nullptr;
#else
  std::mutex m_;
  std::condition_variable readers_cv_;
  std::condition_variable writers_cv_;
  int readers_ = 0;
  int waiting_writers_ = 0;
  bool writer_ = false;
#endif
};

class CondVar {
 public:
  CondVar() = default;
  CondVar(const CondVar&) = delete;
  CondVar& operator=(const CondVar&) = delete;
  void wait(Mutex& mu);  // mu must be held; it is held again on return
  void notifyOne();
  void notifyAll();

 private:
#ifdef _WIN32
  void* cv_ = nullptr;  // CONDITION_VARIABLE, a single pointer initialized to 0
#else
  std::condition_variable cv_;
#endif
};

class Thread {
 public:
  Thread() = default;
  ~Thread();  // joins if still running
  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;
  void start(std::function<void()> fn);
  void join();

 private:
#ifdef _WIN32
  void* handle_ = nullptr;
#else
  std::thread t_;
#endif
};

class LockGuard {
 public:
  explicit LockGuard(Mutex& mu) : mu_(mu) { mu_.lock(); }
  ~LockGuard() { mu_.unlock(); }
  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;

 private:
  Mutex& mu_;
};

class ExclusiveLock {
 public:
  explicit ExclusiveLock(SharedMutex& mu) : mu_(mu) { mu_.lock(); }
  ~ExclusiveLock() { mu_.unlock(); }
  ExclusiveLock(const ExclusiveLock&) = delete;
  ExclusiveLock& operator=(const ExclusiveLock&) = delete;

 private:
  SharedMutex& mu_;
};

class SharedLock {
 public:
  explicit SharedLock(SharedMutex& mu) : mu_(mu) { mu_.lockShared(); }
  ~SharedLock() { mu_.unlockShared(); }
  SharedLock(const SharedLock&) = delete;
  SharedLock& operator=(const SharedLock&) = delete;

 private:
  SharedMutex& mu_;
};

}  // namespace pkv
