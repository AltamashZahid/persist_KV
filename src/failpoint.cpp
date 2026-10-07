#include "persistkv/failpoint.h"

#include <atomic>
#include <cstdlib>

namespace pkv {
namespace failpoint {

namespace {
// Set once before the workload starts; hit() may then run on any thread.
std::string g_name;
std::atomic<long> g_countdown(0);
std::atomic<long> g_hits(0);
}  // namespace

void set(const std::string& name, long countdown) {
  g_name = name;
  g_countdown = countdown;
}

void clear() {
  g_name.clear();
  g_countdown = 0;
}

bool hit(const char* name) {
  g_hits++;
  if (g_name.empty() || (g_name != "any" && g_name != name)) return false;
  return g_countdown.fetch_sub(1) == 1;
}

long hitCount() { return g_hits; }

void crash() { std::_Exit(kCrashExitCode); }

}  // namespace failpoint
}  // namespace pkv
