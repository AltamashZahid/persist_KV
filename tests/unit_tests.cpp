// Unit tests for PersistKV. Each test gets its own directory under test_data/.

#include <algorithm>
#include <atomic>
#include <memory>
#include <cstdio>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "persistkv/crc32.h"
#include "persistkv/db.h"
#include "persistkv/file.h"
#include "persistkv/sync.h"

using namespace pkv;

// --- Minimal test framework -------------------------------------------------

struct TestCase {
  const char* name;
  void (*fn)();
};

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

#define TEST(name)                                  \
  static void name();                               \
  static Registrar registrar_##name(#name, name);   \
  static void name()

#define CHECK(cond)                                                                                  \
  do {                                                                                               \
    if (!(cond)) {                                                                                   \
      throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #cond); \
    }                                                                                                \
  } while (0)

#define CHECK_THROWS(expr, Type)  \
  do {                            \
    bool threw = false;           \
    try {                         \
      expr;                       \
    } catch (const Type&) {       \
      threw = true;               \
    }                             \
    CHECK(threw && #expr);        \
  } while (0)

#define CHECK_HEALTHY(db)                                         \
  do {                                                            \
    std::string err = (db).checkIntegrity();                      \
    if (!err.empty()) throw std::runtime_error("integrity: " + err); \
  } while (0)

// --- Helpers -----------------------------------------------------------------

std::string freshDir(const std::string& name) {
  makeDir("test_data");
  std::string dir = "test_data/" + name;
  makeDir(dir);
  DB::destroy(dir);
  return dir;
}

std::string key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "key%08d", i);
  return buf;
}

std::string value(int i) { return "value-" + std::to_string(i) + std::string(i % 120, 'x'); }

Options fastOptions() {
  Options o;
  o.sync_writes = false;  // keep unit tests fast; crash_test covers fsync paths
  return o;
}

std::map<std::string, std::string> dump(DB& db) {
  std::map<std::string, std::string> out;
  db.scan("", "", [&](const std::string& k, const std::string& v) {
    out[k] = v;
    return true;
  });
  return out;
}

// --- Tests -------------------------------------------------------------------

TEST(crc32_known_vector) {
  CHECK(crc32("123456789", 9) == 0xCBF43926u);
  CHECK(crc32("", 0) == 0);
  // Chaining must equal a single pass.
  CHECK(crc32("6789", 4, crc32("12345", 5)) == 0xCBF43926u);
}

TEST(basic_put_get_update_delete) {
  DB db(freshDir("basic"), fastOptions());
  std::string v;
  CHECK(!db.get("a", &v));

  db.put("a", "1");
  db.put("b", "2");
  CHECK(db.get("a", &v) && v == "1");
  CHECK(db.size() == 2);

  db.put("a", "one");  // overwrite does not change the count
  CHECK(db.get("a", &v) && v == "one");
  CHECK(db.size() == 2);

  CHECK(db.remove("a"));
  CHECK(!db.remove("a"));
  CHECK(!db.get("a", &v));
  CHECK(db.size() == 1);

  db.put("empty", "");
  CHECK(db.get("empty", &v) && v.empty());
  CHECK_HEALTHY(db);
}

TEST(rejects_invalid_keys_and_values) {
  DB db(freshDir("invalid"), fastOptions());
  CHECK_THROWS(db.put("", "x"), Error);
  CHECK_THROWS(db.put(std::string(kMaxKeySize + 1, 'k'), "x"), Error);
  CHECK_THROWS(db.put("k", std::string(kMaxValueSize + 1, 'v')), Error);
  db.put(std::string(kMaxKeySize, 'k'), std::string(kMaxValueSize, 'v'));  // exact limits are fine
  CHECK(db.size() == 1);
}

TEST(random_ops_match_std_map) {
  Options o = fastOptions();
  o.cache_pages = 32;  // force evictions and disk reads
  o.checkpoint_dirty_pages = 64;
  DB db(freshDir("random_ops"), o);
  std::map<std::string, std::string> model;
  std::mt19937 rng(12345);

  for (int i = 0; i < 30000; i++) {
    std::string k = key(rng() % 5000);
    if (rng() % 3 == 0) {
      CHECK(db.remove(k) == (model.erase(k) == 1));
    } else {
      std::string v = value(i);
      db.put(k, v);
      model[k] = v;
    }
    if (i % 5000 == 0) CHECK_HEALTHY(db);
  }
  CHECK_HEALTHY(db);
  CHECK(db.size() == model.size());
  CHECK(dump(db) == model);
  for (auto& kv : model) {
    std::string v;
    CHECK(db.get(kv.first, &v) && v == kv.second);
  }
}

TEST(grow_then_shrink_to_empty_and_reuse_pages) {
  DB db(freshDir("grow_shrink"), fastOptions());
  const int n = 20000;
  for (int i = 0; i < n; i++) db.put(key(i), value(i));
  CHECK_HEALTHY(db);
  CHECK(db.stats().height >= 3);
  uint32_t pages_when_full = db.stats().pages;

  std::vector<int> order(n);
  for (int i = 0; i < n; i++) order[i] = i;
  std::shuffle(order.begin(), order.end(), std::mt19937(7));
  for (int i = 0; i < n; i++) {
    CHECK(db.remove(key(order[i])));
    if (i % 4000 == 0) CHECK_HEALTHY(db);
  }
  CHECK_HEALTHY(db);
  CHECK(db.size() == 0);
  CHECK(db.stats().height == 1);

  // Freed pages go on the free list and are reused, so the file does not grow.
  for (int i = 0; i < n; i++) db.put(key(i), value(i));
  CHECK_HEALTHY(db);
  CHECK(db.stats().pages <= pages_when_full);
}

TEST(range_scan_matches_std_map) {
  DB db(freshDir("range_scan"), fastOptions());
  std::map<std::string, std::string> model;
  for (int i = 0; i < 6000; i += 3) {
    db.put(key(i), value(i));
    model[key(i)] = value(i);
  }

  std::mt19937 rng(99);
  for (int t = 0; t < 200; t++) {
    int a = rng() % 6500, b = rng() % 6500;
    if (a > b) std::swap(a, b);
    std::string lo = key(a), hi = key(b);

    std::vector<std::string> got, want;
    db.scan(lo, hi, [&](const std::string& k, const std::string&) {
      got.push_back(k);
      return true;
    });
    for (auto it = model.lower_bound(lo); it != model.end() && it->first <= hi; ++it) want.push_back(it->first);
    CHECK(got == want);
  }

  // Early termination.
  int seen = 0;
  db.scan(key(0), "", [&](const std::string&, const std::string&) { return ++seen < 10; });
  CHECK(seen == 10);
}

TEST(clean_reopen_needs_no_replay) {
  std::string dir = freshDir("reopen");
  {
    DB db(dir, fastOptions());
    for (int i = 0; i < 5000; i++) db.put(key(i), value(i));
  }
  DB db(dir, fastOptions());
  CHECK(db.stats().wal_records_replayed == 0);
  CHECK(db.size() == 5000);
  std::string v;
  CHECK(db.get(key(4321), &v) && v == value(4321));
  CHECK_HEALTHY(db);
}

TEST(crash_before_checkpoint_replays_wal) {
  std::string dir = freshDir("replay");
  Options o = fastOptions();
  o.checkpoint_wal_bytes = 1ull << 40;  // never checkpoint on our own
  o.checkpoint_dirty_pages = 1u << 30;
  {
    DB db(dir, o);
    for (int i = 0; i < 3000; i++) db.put(key(i), value(i));
    for (int i = 0; i < 3000; i += 2) db.remove(key(i));
    db.abandonForTesting();  // data file still holds the empty tree
  }
  DB db(dir, o);
  CHECK(db.stats().wal_records_replayed == 4500);
  CHECK(db.size() == 1500);
  std::string v;
  CHECK(!db.get(key(10), &v));
  CHECK(db.get(key(11), &v) && v == value(11));
  CHECK_HEALTHY(db);
}

TEST(torn_wal_tail_is_discarded) {
  std::string dir = freshDir("torn_wal");
  {
    DB db(dir, fastOptions());
    for (int i = 0; i < 100; i++) db.put(key(i), value(i));
    db.abandonForTesting();
  }
  {
    // Simulate a crash halfway through appending one more record.
    File wal;
    wal.open(dir + "/" + DB::kWalFile);
    const char garbage[] = "\x40\x00\x00\x00 half-written record...";
    wal.writeAt(wal.size(), garbage, sizeof garbage - 1);
  }
  DB db(dir, fastOptions());
  CHECK(db.size() == 100);
  CHECK(db.stats().wal_bytes_truncated == sizeof("\x40\x00\x00\x00 half-written record...") - 1);
  CHECK_HEALTHY(db);
}

TEST(corrupted_page_is_detected) {
  std::string dir = freshDir("corrupt_page");
  {
    DB db(dir, fastOptions());
    for (int i = 0; i < 2000; i++) db.put(key(i), value(i));
  }
  {
    // Flip one byte in the middle of page 1 (the leftmost leaf).
    File data;
    data.open(dir + "/" + DB::kDataFile);
    char c;
    data.readAt(kPageSize + 100, &c, 1);
    c ^= 0x5A;
    data.writeAt(kPageSize + 100, &c, 1);
  }
  DB db(dir, fastOptions());
  CHECK_THROWS(dump(db), CorruptionError);
}

TEST(large_values_use_overflow_pages) {
  std::string dir = freshDir("overflow");
  // Sizes around every boundary: inline limit, one overflow page, several.
  const size_t sizes[] = {0, 1, kMaxInlineValue, kMaxInlineValue + 1, kOverflowPayload, kOverflowPayload + 1,
                          3 * kOverflowPayload, 100000, kMaxValueSize};
  auto valueOf = [](size_t n, char seed) {
    std::string v(n, 'a');
    for (size_t i = 0; i < n; i++) v[i] = static_cast<char>(seed + i * 31 % 251);
    return v;
  };
  {
    DB db(dir, fastOptions());
    int i = 0;
    for (size_t n : sizes) db.put("big" + std::to_string(i++), valueOf(n, 'x'));
    CHECK_HEALTHY(db);
    uint32_t pages_with_values = db.stats().pages;
    CHECK(pages_with_values > kMaxValueSize / kPageSize);  // the 1 MiB value really is out of line

    // Overwrite large -> small -> large; chains must be freed and reused.
    for (int round = 0; round < 3; round++) {
      db.put("big8", "tiny");
      CHECK_HEALTHY(db);
      db.put("big8", valueOf(kMaxValueSize, 'y'));
      CHECK_HEALTHY(db);
    }
    CHECK(db.stats().pages <= pages_with_values + 1);
  }
  DB db(dir, fastOptions());  // and everything survives a reopen
  int i = 0;
  for (size_t n : sizes) {
    std::string v;
    CHECK(db.get("big" + std::to_string(i), &v));
    CHECK(v == (i == 8 ? valueOf(kMaxValueSize, 'y') : valueOf(n, 'x')));
    i++;
  }
  for (int j = 0; j < i; j++) CHECK(db.remove("big" + std::to_string(j)));
  CHECK_HEALTHY(db);  // includes the check that no overflow page leaked
}

TEST(variable_size_keys_and_values_match_std_map) {
  Options o = fastOptions();
  o.cache_pages = 64;
  o.checkpoint_dirty_pages = 128;
  DB db(freshDir("variable_sizes"), o);
  std::map<std::string, std::string> model;
  std::mt19937 rng(4242);
  auto randomKey = [&] {
    // Few distinct prefixes so keys collide often; lengths 1..kMaxKeySize.
    std::string k = std::to_string(rng() % 3000);
    k.resize(1 + rng() % kMaxKeySize, static_cast<char>('a' + k.size() % 26));
    return k;
  };

  // Phase 1 grows the tree (2 puts per delete); phase 2 shrinks it (3
  // deletes per put), which drives merges and redistribution between
  // siblings whose keys have very different lengths.
  for (int phase = 0; phase < 2; phase++) {
    for (int i = 0; i < 25000; i++) {
      bool del = phase == 0 ? rng() % 3 == 0 : rng() % 4 != 0;
      if (del && !model.empty()) {
        // Delete an existing key most of the time so the tree really shrinks.
        auto it = model.lower_bound(randomKey());
        if (it == model.end()) it = model.begin();
        std::string k = it->first;
        CHECK(db.remove(k));
        model.erase(it);
      } else {
        // Mostly small values, some just past the inline limit, a few big.
        std::string k = randomKey();
        size_t len = rng() % 10 == 0 ? rng() % 3000 : rng() % 64;
        std::string v(len, static_cast<char>('A' + i % 26));
        db.put(k, v);
        model[k] = v;
      }
      if (i % 2500 == 0) CHECK_HEALTHY(db);
    }
    CHECK_HEALTHY(db);
    CHECK(db.size() == model.size());
    CHECK(dump(db) == model);
  }

  // Finally delete everything in random order.
  std::vector<std::string> rest;
  for (auto& kv : model) rest.push_back(kv.first);
  std::shuffle(rest.begin(), rest.end(), rng);
  for (size_t i = 0; i < rest.size(); i++) {
    CHECK(db.remove(rest[i]));
    if (i % 500 == 0) CHECK_HEALTHY(db);
  }
  CHECK_HEALTHY(db);
  CHECK(db.size() == 0 && db.stats().height == 1);
}

TEST(small_entries_pack_densely) {
  DB db(freshDir("dense"), fastOptions());
  const int n = 20000;
  for (int i = 0; i < n; i++) db.put(key(i), "v" + std::to_string(i));
  CHECK_HEALTHY(db);
  // ~25 bytes per entry: well over 100 entries per 4 KB leaf, so 20000
  // entries need a couple of hundred pages, not thousands.
  CHECK(db.stats().pages < 400);
}

TEST(write_batch_applies_atomically) {
  std::string dir = freshDir("batch");
  {
    DB db(dir, fastOptions());
    db.put("before", "1");
    WriteBatch batch;
    for (int i = 0; i < 100; i++) batch.put(key(i), value(i));
    batch.remove("before");
    db.write(batch);
    CHECK(db.size() == 100);
    CHECK(!db.get("before", nullptr));
    db.abandonForTesting();
  }
  DB db(dir, fastOptions());  // the batch is replayed as one unit
  CHECK(db.size() == 100);
  CHECK_HEALTHY(db);
}

TEST(torn_write_batch_is_discarded_entirely) {
  std::string dir = freshDir("torn_batch");
  uint64_t wal_size;
  {
    DB db(dir, fastOptions());
    db.put("before", "1");
    WriteBatch batch;
    for (int i = 0; i < 100; i++) batch.put(key(i), value(i));
    db.write(batch);
    wal_size = db.stats().wal_bytes;
    db.abandonForTesting();
  }
  {
    // Cut the last record (the batch) short, as a crash mid-append would.
    File wal;
    wal.open(dir + "/" + DB::kWalFile);
    wal.truncate(wal_size - 10);
  }
  DB db(dir, fastOptions());
  CHECK(db.size() == 1);  // "before" survives, none of the 100 batch puts do
  CHECK(db.get("before", nullptr));
  CHECK(!db.get(key(0), nullptr));
  CHECK_HEALTHY(db);
}

TEST(error_during_write_puts_db_in_failed_state) {
  std::string dir = freshDir("failed_state");
  Options o = fastOptions();
  o.cache_pages = 16;  // so the corrupted page has to be read from disk
  {
    DB db(dir, o);
    for (int i = 0; i < 3000; i++) db.put(key(i), value(i));
  }
  {
    File data;
    data.open(dir + "/" + DB::kDataFile);
    char c;
    data.readAt(kPageSize + 100, &c, 1);  // page 1 is the leftmost leaf
    c ^= 0x5A;
    data.writeAt(kPageSize + 100, &c, 1);
  }
  {
    DB db(dir, o);
    CHECK_THROWS(db.put(key(0), "new"), CorruptionError);  // logged, then failed to apply
    CHECK_THROWS(db.get(key(1), nullptr), Error);          // every later call is refused
  }
  // The data file was never checkpointed in the failed state; the put is
  // still in the WAL, and replaying it hits the same corrupt page.
  CHECK_THROWS(DB(dir, o), CorruptionError);
}

TEST(concurrent_readers_writers_and_background_checkpoints) {
  std::string dir = freshDir("concurrent");
  Options o = fastOptions();
  o.cache_pages = 64;
  o.checkpoint_dirty_pages = 48;  // checkpoint constantly, concurrently with everything else
  o.checkpoint_wal_bytes = 64 * 1024;

  const int kWriters = 4, kReaders = 4, kOpsPerWriter = 3000, kKeysPerWriter = 400;
  std::vector<std::map<std::string, std::string>> models(kWriters);
  std::atomic<bool> writers_done(false);
  std::atomic<int> errors(0);
  std::atomic<long> reads(0);
  auto writerKey = [](int w, unsigned i) { return "w" + std::to_string(w) + "-" + std::to_string(i % 400); };

  {
    DB db(dir, o);
    std::vector<std::unique_ptr<Thread>> threads;
    for (int w = 0; w < kWriters; w++) {
      threads.emplace_back(new Thread());
      threads.back()->start([&, w] {
        std::mt19937 rng(w);
        auto& model = models[w];
        for (int i = 0; i < kOpsPerWriter; i++) {
          std::string k = writerKey(w, rng() % kKeysPerWriter);
          if (rng() % 4 == 0) {
            db.remove(k);
            model.erase(k);
          } else {
            // Every value starts with its own key, so readers can detect a
            // torn or mismatched read. Some are large enough to overflow.
            std::string v = k + "#" + std::to_string(i) + std::string(rng() % 8 == 0 ? 700 + rng() % 3000 : rng() % 50, '.');
            db.put(k, v);
            model[k] = v;
          }
        }
      });
    }
    for (int r = 0; r < kReaders; r++) {
      threads.emplace_back(new Thread());
      threads.back()->start([&, r] {
        std::mt19937 rng(100 + r);
        std::string v;
        while (!writers_done) {
          std::string k = writerKey(rng() % kWriters, rng() % kKeysPerWriter);
          if (db.get(k, &v) && v.compare(0, k.size() + 1, k + "#") != 0) errors++;
          if (rng() % 50 == 0) {
            std::string prev;
            db.scan(k, "", [&](const std::string& sk, const std::string& sv) {
              if (!prev.empty() && !(prev < sk)) errors++;
              if (sv.compare(0, sk.size() + 1, sk + "#") != 0) errors++;
              prev = sk;
              return prev.size() < 1000 && rng() % 64 != 0;
            });
          }
          reads++;
        }
      });
    }
    for (int w = 0; w < kWriters; w++) threads[w]->join();
    writers_done = true;
    for (auto& t : threads) t->join();

    CHECK(errors == 0);
    CHECK(reads > 0);
    CHECK_HEALTHY(db);
    CHECK(db.stats().checkpoints > 1);
    std::map<std::string, std::string> all;
    for (auto& m : models) all.insert(m.begin(), m.end());
    CHECK(dump(db) == all);
    db.abandonForTesting();  // and recovery must reproduce exactly this state
  }
  DB db(dir, o);
  std::map<std::string, std::string> all;
  for (auto& m : models) all.insert(m.begin(), m.end());
  CHECK(dump(db) == all);
  CHECK_HEALTHY(db);
}

TEST(group_commit_shares_fsyncs_between_threads) {
  std::string dir = freshDir("group_commit");
  Options o;  // durable: every commit is fsynced
  const int kThreads = 8, kPerThread = 150;
  {
    DB db(dir, o);
    std::vector<std::unique_ptr<Thread>> threads;
    for (int t = 0; t < kThreads; t++) {
      threads.emplace_back(new Thread());
      threads.back()->start([&, t] {
        for (int i = 0; i < kPerThread; i++) db.put(key(t * kPerThread + i), value(i));
      });
    }
    for (auto& t : threads) t->join();
    DBStats s = db.stats();
    CHECK(s.commit_batches == kThreads * kPerThread);
    CHECK(s.commit_groups < s.commit_batches);  // several commits shared one fsync
    std::printf("         (%s commits in %s fsyncs)\n", std::to_string(s.commit_batches).c_str(),
                std::to_string(s.commit_groups).c_str());
  }
  DB db(dir, o);
  CHECK(db.size() == kThreads * kPerThread);
  CHECK_HEALTHY(db);
}

int main(int argc, char** argv) {
  std::string filter = argc > 1 ? argv[1] : "";
  int passed = 0, failed = 0;
  for (const TestCase& t : registry()) {
    if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos) continue;
    try {
      t.fn();
      std::cout << "[PASS] " << t.name << "\n";
      passed++;
    } catch (const std::exception& e) {
      std::cout << "[FAIL] " << t.name << ": " << e.what() << "\n";
      failed++;
    }
  }
  std::cout << "\n" << passed << " passed, " << failed << " failed\n";
  return failed == 0 ? 0 : 1;
}
