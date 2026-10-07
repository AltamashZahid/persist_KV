#include "persistkv/failpoint.h"

#include <cstdlib>

namespace pkv {
namespace failpoint {

namespace {
std::string g_name;
long g_countdown = 0;
long g_hits = 0;
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
  return --g_countdown == 0;
}

long hitCount() { return g_hits; }

void crash() { std::_Exit(kCrashExitCode); }

}  // namespace failpoint
}  // namespace pkv
