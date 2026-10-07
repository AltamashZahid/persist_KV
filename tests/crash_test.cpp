// Crash-recovery torture test.
//
// The parent re-runs this binary as a child process with one failpoint armed.
// The child performs a deterministic sequence of puts/deletes and records
// each acknowledged operation in an "ack" file; the failpoint kills it at a
// precise moment (mid-WAL-append, mid-checkpoint, torn page write, ...).
// The parent then reopens the database, letting recovery run, and checks:
//   * the tree passes the full integrity check, and
//   * its contents equal the model after every acknowledged operation
//     (optionally plus the single in-flight operation, which may or may not
//     have become durable before the crash).

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "persistkv/db.h"
#include "persistkv/failpoint.h"
#include "persistkv/file.h"

#ifndef _WIN32
#include <sys/wait.h>
#endif

using namespace pkv;

namespace {

constexpr int kNumOps = 800;

struct Op {
  bool is_put;
  std::string key;
  std::string value;
};

std::vector<Op> makeOps() {
  std::mt19937 rng(2024);
  std::vector<Op> ops;
  for (int i = 0; i < kNumOps; i++) {
    char key[32];
    std::snprintf(key, sizeof key, "k%05u", static_cast<unsigned>(rng() % 400));
    if (i > 50 && rng() % 4 == 0) {
      ops.push_back({false, key, ""});
    } else {
      ops.push_back({true, key, "v" + std::to_string(i) + std::string(rng() % 150, '#')});
    }
  }
  return ops;
}

Options childOptions() {
  Options o;
  o.sync_writes = true;
  o.cache_pages = 16;
  o.checkpoint_wal_bytes = 8 * 1024;  // checkpoint often so checkpoint failpoints fire
  o.checkpoint_dirty_pages = 24;
  return o;
}

int runChild(const std::string& dir, const std::string& fp, long count) {
  std::vector<Op> ops = makeOps();
  FILE* ack = std::fopen((dir + "/ack.txt").c_str(), "w");
  failpoint::set(fp, count);
  DB db(dir, childOptions());
  for (int i = 0; i < kNumOps; i++) {
    if (ops[i].is_put) {
      db.put(ops[i].key, ops[i].value);
    } else {
      db.remove(ops[i].key);
    }
    std::fprintf(ack, "%d\n", i);  // op i is acknowledged
    std::fflush(ack);
  }
  std::fclose(ack);
  return 0;
}

int readAcked(const std::string& dir) {
  std::ifstream in(dir + "/ack.txt");
  int last = -1, x;
  while (in >> x) last = x;
  return last + 1;
}

void applyOp(std::map<std::string, std::string>& m, const Op& op) {
  if (op.is_put) {
    m[op.key] = op.value;
  } else {
    m.erase(op.key);
  }
}

bool runScenario(const std::string& self, const std::string& fp, long count) {
  std::string dir = "test_data/crash_" + fp + "_" + std::to_string(count);
  for (char& c : dir) {
    if (c == '.') c = '_';
  }
  makeDir("test_data");
  makeDir(dir);
  DB::destroy(dir);
  removeFile(dir + "/ack.txt");

  std::string cmd = "\"" + self + "\" child " + dir + " " + fp + " " + std::to_string(count);
#ifdef _WIN32
  cmd = "\"" + cmd + "\"";  // cmd.exe strips one level of outer quotes
#endif
  int rc = std::system(cmd.c_str());
#ifndef _WIN32
  if (WIFEXITED(rc)) rc = WEXITSTATUS(rc);
#endif
  bool crashed = rc == failpoint::kCrashExitCode;

  int acked = readAcked(dir);
  std::vector<Op> ops = makeOps();
  std::map<std::string, std::string> expected;
  for (int i = 0; i < acked; i++) applyOp(expected, ops[i]);
  std::map<std::string, std::string> with_inflight = expected;
  if (acked < kNumOps) applyOp(with_inflight, ops[acked]);

  std::string verdict;
  DBStats st;
  try {
    DB db(dir, childOptions());
    st = db.stats();
    std::string err = db.checkIntegrity();
    std::map<std::string, std::string> actual;
    db.scan("", "", [&](const std::string& k, const std::string& v) {
      actual[k] = v;
      return true;
    });
    if (!err.empty()) {
      verdict = "integrity: " + err;
    } else if (actual != expected && actual != with_inflight) {
      verdict = "contents differ from acknowledged state (" + std::to_string(actual.size()) + " keys, expected " +
                std::to_string(expected.size()) + ")";
    }
  } catch (const std::exception& e) {
    verdict = std::string("recovery threw: ") + e.what();
  }

  std::printf("[%s] %-22s #%-4ld %s acked=%-4d replayed=%-4s torn_wal_bytes=%-3s dwb_repair=%s %s\n",
              verdict.empty() ? "PASS" : "FAIL", fp.c_str(), count, crashed ? "crashed " : "no-crash", acked,
              std::to_string(st.wal_records_replayed).c_str(),
              std::to_string(st.wal_bytes_truncated).c_str(), st.recovered_from_doublewrite ? "yes" : "no ",
              verdict.c_str());
  return verdict.empty();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 5 && std::string(argv[1]) == "child") {
    return runChild(argv[2], argv[3], std::atol(argv[4]));
  }

  struct Scenario {
    const char* failpoint;
    long count;
  };
  const Scenario scenarios[] = {
      {"wal.torn", 1},           {"wal.torn", 137},         {"wal.torn", 611},
      {"db.after_wal", 42},      {"db.after_wal", 500},     {"ckpt.after_dwb", 1},
      {"ckpt.after_dwb", 3},     {"ckpt.after_dwb", 9},     {"ckpt.torn_page", 2},
      {"ckpt.torn_page", 30},    {"ckpt.torn_page", 70},   {"ckpt.after_data", 4},
      {"ckpt.before_wal_reset", 5}, {"none", 1},
  };

  int failed = 0;
  for (const Scenario& s : scenarios) {
    if (!runScenario(argv[0], s.failpoint, s.count)) failed++;
  }
  std::printf("\n%d scenario(s) failed\n", failed);
  return failed == 0 ? 0 : 1;
}
