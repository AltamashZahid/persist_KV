# PersistKV

[![CI](https://github.com/AltamashZahid/persist_KV/actions/workflows/ci.yml/badge.svg)](https://github.com/AltamashZahid/persist_KV/actions/workflows/ci.yml)

A crash-safe, thread-safe key-value storage engine in C++14. It is built on a
disk-resident B+Tree with variable-size nodes and overflow pages, a write-ahead log with
group commit, page checksums, and background checkpoints protected by a doublewrite
buffer. It has no dependencies beyond the standard library, and it builds with any
C++14 compiler (tested with MinGW g++ 6.3, GCC, Clang and MSVC).

New to these ideas? **[GUIDE.md](GUIDE.md)** explains every concept from first principles:
fsync and torn writes, B+Tree splits and merges, WAL and LSNs, group commit, checkpoints,
recovery, steal/force, and interview questions with answers.

```cpp
pkv::DB db("mydb");
db.put("user:1", "alice");
std::string v;
db.get("user:1", &v);
db.scan("user:", "user:~", [](const std::string& k, const std::string& v) { return true; });

pkv::WriteBatch batch;              // all-or-nothing, one fsync
batch.put("acct:a", "90");
batch.put("acct:b", "110");
db.write(batch);
```

## Features

- **B+Tree index:** point lookups, ordered range scans over linked leaves, and
  O(log n) insert and delete. Nodes are sized **in bytes**: they split above one page
  and merge or redistribute below 25% full, so small entries pack hundreds per page.
- **Keys up to 128 B, values up to 1 MiB.** Values over 512 B live in **overflow page
  chains**, and freed pages are reused through a free list.
- **Durability:** a write-ahead log with LSNs and CRC32 on every record. A torn tail is
  detected and discarded on recovery.
- **Atomic batches:** a `WriteBatch` is one WAL record, so after a crash all of it is
  present or none of it is.
- **Torn-page safety:** checkpoints go through a **doublewrite buffer**, and every page
  carries a CRC32.
- **Concurrency:** readers run in parallel. Concurrent writers are combined by **group
  commit** into one fsync. **Checkpoints run on a background thread**, and their disk
  I/O does not block reads or writes.
- **Fail-safe errors:** if applying a change fails midway, the DB refuses further calls
  and never writes the half-applied state to disk. Reopening recovers from the WAL.

## Build and run

```sh
mingw32-make          # Windows (MinGW); use `make` on Linux/macOS
# or: cmake -S . -B build && cmake --build build && ctest --test-dir build

./unit_tests          # 18 tests: model comparisons, overflow, batches, concurrency
./crash_test          # 14 targeted crash points + 40 randomized crash runs
./crash_test fuzz 500 # 500 randomized crash runs (CI runs this on every push)
./bench 50000         # throughput numbers
./kvcli mydb          # interactive shell: put/get/del/scan/stats/check
```

CI builds and tests on Linux (GCC with AddressSanitizer + UBSan, Clang, and Clang with
ThreadSanitizer) and Windows (MSVC).

## Architecture

```
PersistKV
 ├── DB          public API, group commit, recovery, checkpoint thread    src/db.cpp
 ├── BTree       byte-sized B+Tree nodes, overflow chains, range scan     src/btree.cpp
 ├── Pager       LRU page cache, free list, 3-phase checkpoint + dwb      src/pager.cpp
 ├── Wal         batch records with CRC32, torn-tail truncation           src/wal.cpp
 ├── sync        mutex / rwlock / condvar / thread (Win32 or std)         src/sync.cpp
 └── File        positional I/O + fsync (POSIX) / _commit (Windows)       src/file.cpp
```

### On-disk files

| File | Purpose |
|------|---------|
| `data.db` | Array of 4 KB pages. Page 0 is the meta page (root, page count, free-list head, checkpoint LSN, key count). The rest are leaf, internal, overflow or free pages. |
| `wal-0.log`, `wal-1.log` | Redo log: one record per committed batch. There are two files so that a running checkpoint can retire one while new commits go to the other. |
| `dwb.log` | Doublewrite buffer. It is non-empty only while a checkpoint is being written. |

### Write path (group commit)
1. A writer joins the commit queue. The writer at the front becomes the **leader**. It
   appends every queued batch to the active WAL, issues **one fsync** for all of them,
   then applies them to the tree in memory under the exclusive tree lock.
2. The leader wakes the other writers, whose batches are now durable and applied.
3. When the WAL or the dirty-page count crosses a threshold, the leader signals the
   **checkpoint thread**. If the backlog reaches 4× the threshold, writers wait for it
   (backpressure).

### Checkpoint (background thread)
1. **Snapshot**, briefly holding the WAL and tree locks: copy the dirty pages and the
   meta page, mark those pages clean but **pinned** in the cache, fsync the active WAL,
   and switch new appends to the other WAL file.
2. **Write**, with no tree lock held: copy the pages to `dwb.log` and fsync, write them
   in place in `data.db` and fsync, then truncate `dwb.log`.
3. **Finish:** empty the retired WAL file and unpin the pages.

### Recovery (on every open)
1. If `dwb.log` is complete and passes its CRC, copy its pages into `data.db`. This
   repairs any torn pages. An incomplete doublewrite buffer means `data.db` was never
   touched.
2. Load and verify the meta page.
3. Read both WAL files, truncating any torn tail. Sort the records by LSN and redo
   those newer than the meta page's `checkpoint_lsn`.
4. Checkpoint, then empty both logs.

## Testing

| Test | What it proves |
|---|---|
| Model tests | 30K–75K random operations compared against `std::map`, with full integrity checks along the way (sorted keys, separator bounds, byte fill, equal leaf depth, leaf chain, overflow chains, and no leaked pages) |
| Overflow and sizing | Values at every size boundary up to 1 MiB; chains freed and reused; dense packing of small entries |
| Batches | Atomic commit, and a batch torn mid-record is discarded entirely |
| Concurrency | 4 writers + 4 readers with constant background checkpoints; readers verify they never see a torn value; recovery afterwards. ThreadSanitizer in CI |
| Crash: targeted | The engine is killed at 14 exact points (torn WAL record, torn page, between every checkpoint step). Recovery must reproduce exactly the acknowledged operations |
| Crash: fuzzer | Random workloads (single ops, batches, overflow-sized values) crashed at random points. Half are crashed **again during recovery** |

## Performance

MinGW g++ 6.3 -O2, Windows 11, 50K keys, 100-byte values:

| Workload | ops/s |
|---|---|
| random get | ~130,000–160,000 |
| range scan, 100 keys each | ~15,000–18,000 scans |
| random put, no fsync per op | ~27,000 |
| random delete | ~20,000 |
| durable put (fsync per commit), 1 thread | ~1,000 |
| durable put, 16 threads (group commit) | ~4,700 (2,000 commits in 236 fsyncs) |
| durable put, `WriteBatch` of 100 | ~14,000 |

## Limitations and next steps

- Writers are serialized through the tree lock. Per-node latching ("latch crabbing")
  would let writers on different subtrees run in parallel.
- No multi-version reads: a long `scan` holds the shared lock and delays writers. MVCC
  snapshots would fix that.
- No prefix compression in internal nodes, and no compression of values.
