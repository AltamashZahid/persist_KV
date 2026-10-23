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
- **Zero-copy reads:** `getRaw`, `multiGetRaw` (a sorted batch of keys; neighbours
  in the same leaf are found without a new descent from the root) and `scanRaw` hand
  out key and value bytes straight from the cached page, which is how
  [PKQL](https://github.com/AltamashZahid/MY-PersistKV) decodes rows without copying.
  Pages are binary-searched through a slot array.
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
- **Python bindings** (pybind11): `import persistkv`. The GIL is released inside the
  engine, so Python threads run in parallel and share group-commit fsyncs.

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
ThreadSanitizer) and Windows (MSVC), and builds and tests the Python module on both.

## Python

```sh
pip install .                       # compiles the engine + bindings into one module
python -m pytest python/tests -q    # 11 tests
```

```python
import persistkv

with persistkv.DB("mydb") as db:
    db.put(b"user:1", b"alice")
    db.get(b"user:1")                 # b"alice", or None
    db.scan_prefix(b"user:")          # [(key, value), ...] in key order
    batch = persistkv.WriteBatch()    # atomic, one fsync
    batch.put(b"acct:a", b"90")
    batch.put(b"acct:b", b"110")
    db.write(batch)
```

Errors raise `persistkv.Error` (with `persistkv.CorruptionError` as a subclass). On
Windows the module needs MSVC (Visual Studio Build Tools), the compiler Python itself is
built with. [GUIDE.md section 20](GUIDE.md#20-using-it-from-python) explains how the
bindings work: the GIL, exception translation, and why `scan` never runs Python code
under the tree lock.

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
| Python | 11 pytest tests for the bindings: persistence, overflow values, atomic batches, threads sharing group commit |

Latest full local run (Windows, MinGW g++ 6.3): the unit tests passed 3 times in a row
(54/54), the targeted crash tests passed 14/14, the 500-run crash fuzzer had 0 failures
(443 runs crashed mid-workload, 209 crashed again during recovery), and pytest passed
11/11.

## Performance

### Running the benchmark

```sh
mingw32-make bench.exe
./bench.exe 50000 C:/Temp/pkvbench    # number of keys, then a scratch directory (wiped first)
```

The benchmark uses 100-byte values and runs: random puts, random gets, range scans,
deleting half the keys (followed by a full integrity check), then durable writes. The
durable writes are measured one at a time, in `WriteBatch`es of 100, and from 1, 4 and
16 threads (group commit). For stable numbers, put the scratch directory outside any
synced folder (OneDrive, Dropbox), close heavy applications, and take the median of 3
runs.

### Results

Setup: Intel Core i5-13500H, 16 GB RAM, Samsung NVMe SSD, Windows 11, MinGW g++ 6.3
`-O2`. Median of 3 runs, 50K keys (all data fits in the 16 MB page cache):

| Workload | Throughput |
|---|---|
| random get | 228,000 ops/s |
| range scan, 100 keys each | 19,500 scans/s |
| random put, no fsync per op | 92,400 ops/s |
| random delete | 30,500 ops/s |
| durable put, 1 thread (one fsync per commit) | 1,440 ops/s |
| durable put, 4 threads (group commit) | 2,640 ops/s (2,000 commits in ~810 fsyncs) |
| durable put, 16 threads (group commit) | 7,380 ops/s (2,000 commits in ~239 fsyncs), **5.1×** one thread |
| durable put, `WriteBatch` of 100 | 85,200 ops/s |

Puts are fast because most of them change only their leaf page, in place: the new
leaf is assembled from byte ranges of the cached page, with no decoding into nodes.
Only splits, merges and values stored in overflow pages take the general path (3.3×
faster random puts and 7.6× faster batched puts than before this was added).

With 200K keys, the data is larger than the page cache, so many operations read from
disk: puts fall to 49,800 ops/s, gets to 19,100 ops/s and deletes to 8,100 ops/s.
The integrity check still passes after every run.

Durable-write numbers depend mostly on how long the disk takes to fsync, so they vary
between machines and between runs. Group commit gave 4.3× to 6.1× over one thread
across runs. Timings vary by about ±20% between runs, which is why the table reports
medians.

## Limitations and next steps

- Writers are serialized through the tree lock. Per-node latching ("latch crabbing")
  would let writers on different subtrees run in parallel.
- No multi-version reads: a long `scan` holds the shared lock and delays writers. MVCC
  snapshots would fix that.
- No prefix compression in internal nodes, and no compression of values.
