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
//     have become durable before the crash). Some operations are batches,
//     which must appear entirely or not at all.
//
// Usage:
//   crash_test                 targeted scenarios + 40 randomized runs
//   crash_test fuzz N [seed]   N randomized runs only

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
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

// One step of the workload: a single put/delete, or an atomic batch.
struct Op {
  WriteBatch batch;
};

std::vector<Op> makeOps(unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<Op> ops(kNumOps);
  for (int i = 0; i < kNumOps; i++) {
    // Most steps are a single operation; one in five is a batch of 2-4.
    int n = rng() % 5 == 0 ? 2 + static_cast<int>(rng() % 3) : 1;
    for (int j = 0; j < n; j++) {
      char key[32];
      std::snprintf(key, sizeof key, "k%05u", static_cast<unsigned>(rng() % 400));
      if (i > 50 && rng() % 4 == 0) {
        ops[i].batch.remove(key);
      } else {
        // Occasionally a value large enough to need overflow pages.
        size_t len = rng() % 20 == 0 ? 600 + rng() % 6000 : rng() % 150;
        ops[i].batch.put(key, "v" + std::to_string(i) + std::string(len, '#'));
      }
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

std::string readFile(const std::string& path) {
  std::ifstream in(path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// --- Child processes ---------------------------------------------------------

// Runs the workload with a failpoint armed. Exits 42 if the failpoint fires.
int runWorkload(const std::string& dir, const std::string& fp, long count, unsigned seed) {
  std::vector<Op> ops = makeOps(seed);
  FILE* ack = std::fopen((dir + "/ack.txt").c_str(), "w");
  failpoint::set(fp, count);
  {
    DB db(dir, childOptions());
    for (int i = 0; i < kNumOps; i++) {
      const WriteBatch& b = ops[i].batch;
      if (b.count() > 1) {
        db.write(b);
      } else if (b.entries()[0].op == WalOp::Put) {
        db.put(b.entries()[0].key, b.entries()[0].value);
      } else {
        db.remove(b.entries()[0].key);
      }
      std::fprintf(ack, "%d\n", i);  // op i is acknowledged
      std::fflush(ack);
    }
  }
  std::fclose(ack);
  std::ofstream(dir + "/hits.txt") << failpoint::hitCount();
  return 0;
}

// Opens (and therefore recovers) the database with a failpoint armed, so the
// crash lands in the middle of recovery itself.
int runRecovery(const std::string& dir, long count) {
  failpoint::set("any", count);
  DB db(dir, childOptions());
  db.abandonForTesting();
  return 0;
}

// --- Parent ------------------------------------------------------------------

std::string self_path;

int spawn(const std::string& args) {
  std::string cmd = "\"" + self_path + "\" " + args;
#ifdef _WIN32
  cmd = "\"" + cmd + "\"";  // cmd.exe strips one level of outer quotes
#endif
  int rc = std::system(cmd.c_str());
#ifndef _WIN32
  if (WIFEXITED(rc)) rc = WEXITSTATUS(rc);
#endif
  return rc;
}

std::string prepareDir(const std::string& name) {
  makeDir("test_data");
  std::string dir = "test_data/" + name;
  makeDir(dir);
  DB::destroy(dir);
  removeFile(dir + "/ack.txt");
  removeFile(dir + "/hits.txt");
  return dir;
}

int readAcked(const std::string& dir) {
  std::ifstream in(dir + "/ack.txt");
  int last = -1, x;
  while (in >> x) last = x;
  return last + 1;
}

void applyOp(std::map<std::string, std::string>& m, const Op& op) {
  for (const WriteBatch::Entry& e : op.batch.entries()) {
    if (e.op == WalOp::Put) {
      m[e.key] = e.value;
    } else {
      m.erase(e.key);
    }
  }
}

// Recovers the database and compares it with the acknowledged model.
// Returns an empty string on success, else what went wrong.
std::string verify(const std::string& dir, unsigned seed, DBStats* stats) {
  int acked = readAcked(dir);
  std::vector<Op> ops = makeOps(seed);
  std::map<std::string, std::string> expected;
  for (int i = 0; i < acked; i++) applyOp(expected, ops[i]);
  std::map<std::string, std::string> with_inflight = expected;
  if (acked < kNumOps) applyOp(with_inflight, ops[acked]);

  try {
    DB db(dir, childOptions());
    *stats = db.stats();
    std::string err = db.checkIntegrity();
    if (!err.empty()) return "integrity: " + err;
    std::map<std::string, std::string> actual;
    db.scan("", "", [&](const std::string& k, const std::string& v) {
      actual[k] = v;
      return true;
    });
    if (actual != expected && actual != with_inflight) {
      return "contents differ from acknowledged state (" + std::to_string(actual.size()) + " keys, expected " +
             std::to_string(expected.size()) + ")";
    }
  } catch (const std::exception& e) {
    return std::string("recovery threw: ") + e.what();
  }
  return "";
}

bool runScenario(const std::string& fp, long count) {
  std::string name = "crash_" + fp + "_" + std::to_string(count);
  for (char& c : name) {
    if (c == '.') c = '_';
  }
  std::string dir = prepareDir(name);
  const unsigned seed = 2024;
  bool crashed = spawn("child " + dir + " " + fp + " " + std::to_string(count) + " " + std::to_string(seed)) ==
                 failpoint::kCrashExitCode;

  DBStats st;
  std::string verdict = verify(dir, seed, &st);
  std::printf("[%s] %-22s #%-4ld %s acked=%-4d replayed=%-4s torn_wal_bytes=%-3s dwb_repair=%s %s\n",
              verdict.empty() ? "PASS" : "FAIL", fp.c_str(), count, crashed ? "crashed " : "no-crash",
              readAcked(dir), std::to_string(st.wal_records_replayed).c_str(),
              std::to_string(st.wal_bytes_truncated).c_str(), st.recovered_from_doublewrite ? "yes" : "no ",
              verdict.c_str());
  return verdict.empty();
}

// Each fuzz run picks a random operation sequence and crashes at a random
// failpoint hit; every other run also crashes once during recovery.
int runFuzz(int iterations, unsigned seed) {
  std::mt19937 rng(seed);

  // Dry run to learn how many failpoint hits a full workload produces.
  std::string probe = prepareDir("fuzz_probe");
  spawn("child " + probe + " none 1 1");
  long total_hits = std::atol(readFile(probe + "/hits.txt").c_str());
  if (total_hits <= 0) {
    std::printf("[FAIL] fuzz: could not measure failpoint hits\n");
    return 1;
  }

  int failed = 0, crashes = 0, recovery_crashes = 0;
  for (int it = 0; it < iterations; it++) {
    unsigned ops_seed = rng();
    long at = 1 + static_cast<long>(rng() % static_cast<unsigned long>(total_hits + total_hits / 10));
    std::string dir = prepareDir("fuzz");
    if (spawn("child " + dir + " any " + std::to_string(at) + " " + std::to_string(ops_seed)) ==
        failpoint::kCrashExitCode) {
      crashes++;
    }

    bool crash_recovery = it % 2 == 1;
    long rec_at = 1 + static_cast<long>(rng() % 8);
    if (crash_recovery && spawn("recover " + dir + " " + std::to_string(rec_at)) == failpoint::kCrashExitCode) {
      recovery_crashes++;
    }

    DBStats st;
    std::string verdict = verify(dir, ops_seed, &st);
    if (!verdict.empty()) {
      failed++;
      std::printf("[FAIL] fuzz run %d: ops_seed=%u crash_at=%ld recovery_crash_at=%ld: %s\n", it, ops_seed, at,
                  crash_recovery ? rec_at : 0L, verdict.c_str());
    }
  }
  std::printf("[%s] fuzz: %d runs, %d crashed mid-workload, %d crashed again during recovery, %d failed\n",
              failed == 0 ? "PASS" : "FAIL", iterations, crashes, recovery_crashes, failed);
  return failed;
}

}  // namespace

int main(int argc, char** argv) {
  self_path = argv[0];
  std::string mode = argc > 1 ? argv[1] : "";

  if (mode == "child" && argc == 6) {
    return runWorkload(argv[2], argv[3], std::atol(argv[4]), static_cast<unsigned>(std::strtoul(argv[5], nullptr, 10)));
  }
  if (mode == "recover" && argc == 4) return runRecovery(argv[2], std::atol(argv[3]));

  // A child whose arguments were mangled (e.g. by wildcard expansion in the C
  // runtime) must never fall through to parent mode, or it would spawn
  // children of its own.
  if (mode == "child" || mode == "recover") {
    std::fprintf(stderr, "crash_test: child launched with unexpected arguments (argc=%d)\n", argc);
    return 2;
  }
  if (mode == "fuzz") {
    int n = argc > 2 ? std::atoi(argv[2]) : 200;
    unsigned seed = argc > 3 ? static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10)) : 1;
    return runFuzz(n, seed) == 0 ? 0 : 1;
  }

  struct Scenario {
    const char* failpoint;
    long count;
  };
  const Scenario scenarios[] = {
      {"wal.torn", 1},           {"wal.torn", 137},         {"wal.torn", 611},
      {"db.after_wal", 42},      {"db.after_wal", 500},     {"ckpt.after_dwb", 1},
      {"ckpt.after_dwb", 3},     {"ckpt.after_dwb", 9},     {"ckpt.torn_page", 2},
      {"ckpt.torn_page", 30},    {"ckpt.torn_page", 70},    {"ckpt.after_data", 4},
      {"ckpt.before_wal_reset", 5}, {"none", 1},
  };

  int failed = 0;
  for (const Scenario& s : scenarios) {
    if (!runScenario(s.failpoint, s.count)) failed++;
  }
  failed += runFuzz(40, 1);
  std::printf("\n%d failure(s)\n", failed);
  return failed == 0 ? 0 : 1;
}
