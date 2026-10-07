// Unit tests for PersistKV. Each test gets its own directory under test_data/.

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "persistkv/crc32.h"
#include "persistkv/db.h"
#include "persistkv/file.h"

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
