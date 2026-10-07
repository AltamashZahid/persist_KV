#pragma once

#include <string>

namespace pkv {
namespace failpoint {

// Exit code used when a failpoint kills the process.
constexpr int kCrashExitCode = 42;

// Arms the failpoint `name` so that its `countdown`-th hit fires (1 = first hit).
// Only one failpoint can be armed at a time; this is a testing hook.
void set(const std::string& name, long countdown);
void clear();

// Returns true exactly once, when the armed failpoint's countdown reaches zero.
bool hit(const char* name);

// Terminates immediately, skipping destructors and atexit handlers, the way
// a kill -9 or power cut would from the engine's point of view.
[[noreturn]] void crash();

inline void crashIfHit(const char* name) {
  if (hit(name)) crash();
}

}  // namespace failpoint
}  // namespace pkv
