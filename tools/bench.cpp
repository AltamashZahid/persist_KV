// Throughput benchmark: bench [num_keys] [dir]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "persistkv/db.h"
#include "persistkv/file.h"

using namespace pkv;

namespace {

using Clock = std::chrono::steady_clock;

std::string key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "user%010d", i);
  return buf;
}

template <typename F>
void timeIt(const char* name, long ops, F&& fn) {
  auto start = Clock::now();
  fn();
  double secs = std::chrono::duration<double>(Clock::now() - start).count();
  std::printf("%-34s %9ld ops %8.3f s %12.0f ops/s\n", name, ops, secs, ops / secs);
}

}  // namespace

int main(int argc, char** argv) {
  int n = argc > 1 ? std::atoi(argv[1]) : 200000;
  std::string dir = argc > 2 ? argv[2] : "bench_data";
  makeDir(dir);
  DB::destroy(dir);

  std::vector<int> ids(n);
  for (int i = 0; i < n; i++) ids[i] = i;
  std::mt19937 rng(1);
  std::shuffle(ids.begin(), ids.end(), rng);
  const std::string value(100, 'v');

  {
    Options o;
    o.sync_writes = false;
    DB db(dir, o);

    timeIt("random put (no fsync/op)", n, [&] {
      for (int id : ids) db.put(key(id), value);
    });
    timeIt("random get", n, [&] {
      std::string v;
      for (int id : ids) {
        if (!db.get(key(id), &v)) std::abort();
      }
    });
    const int scans = 2000, scan_len = 100;
    timeIt("range scan (100 keys each)", scans, [&] {
      long total = 0;
      for (int i = 0; i < scans; i++) {
        db.scan(key(rng() % n), "", [&](const std::string&, const std::string&) { return ++total % scan_len != 0; });
      }
    });
    timeIt("random delete (half the keys)", n / 2, [&] {
      for (int i = 0; i < n / 2; i++) db.remove(key(ids[i]));
    });
    DBStats s = db.stats();
    std::printf("\nheight=%u pages=%u keys=%s checkpoints=%s integrity=%s\n\n", s.height, s.pages,
                std::to_string(s.keys).c_str(), std::to_string(s.checkpoints).c_str(),
                db.checkIntegrity().empty() ? "OK" : "FAILED");
  }

  {
    Options o;
    o.sync_writes = true;
    DB db(dir, o);
    const int m = 500;
    timeIt("durable put (fsync every write)", m, [&] {
      for (int i = 0; i < m; i++) db.put(key(n + i), value);
    });
  }
  return 0;
}
