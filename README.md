# PersistKV

[![CI](https://github.com/AltamashZahid/persist_KV/actions/workflows/ci.yml/badge.svg)](https://github.com/AltamashZahid/persist_KV/actions/workflows/ci.yml)

A crash-safe key-value storage engine in C++14, built on a disk-resident B+Tree, a
write-ahead log, page checksums and a doublewrite buffer. It has no dependencies
beyond the standard library and builds with any C++14 compiler (tested on MinGW g++ 6.3).

New to these ideas? **[GUIDE.md](GUIDE.md)** explains every concept from first principles:
fsync and torn writes, B+Tree splits and merges, WAL and LSNs, checkpoints, recovery,
steal/force, and interview questions with answers.

```
PersistKV
 ├── DB          public API: put / get / remove / scan / checkpoint      src/db.cpp
 ├── BTree       B+Tree over pages: split, borrow, merge, range scan     src/btree.cpp
 ├── Pager       page cache (LRU), free list, checkpoint + doublewrite   src/pager.cpp
 ├── Wal         append-only redo log with per-record CRC32               src/wal.cpp
 └── File        positional I/O + fsync (POSIX) / _commit (Windows)       src/file.cpp
```

## Build and run

```sh
mingw32-make          # Windows (MinGW); use `make` on Linux/macOS
./unit_tests          # 10 correctness tests, including comparisons against std::map
./crash_test          # kills the engine at 14 injected points and verifies recovery
./bench 200000        # throughput numbers
./kvcli mydb          # interactive shell: put/get/del/scan/stats/check
```

## On-disk files

| File      | Purpose |
|-----------|---------|
| `data.db` | Array of 4 KB pages. Page 0 is the meta page (root, page count, free-list head, checkpoint LSN, key count). The other pages are B+Tree nodes or free pages. |
| `wal.log` | Logical redo records `[crc][len][lsn][op][klen][vlen][key][value]` for every write since the last checkpoint. |
| `dwb.log` | Doublewrite buffer. It is non-empty only while a checkpoint is in progress. |

Every page begins with a 16-byte header `[crc32][type][nkeys][next]`, and the CRC covers the
whole page.

## Write path

1. `put(k, v)` assigns the next **LSN** and appends a WAL record, then **fsyncs** it.
   When `put` returns, the write is durable.
2. The change is applied to the B+Tree **in the page cache**. Dirty pages are pinned in
   memory, so the data file never holds a half-applied operation.
3. Once the WAL or the dirty-page count passes a threshold, a **checkpoint** runs:
   1. All dirty pages and the meta page are written to `dwb.log` as one checksummed
      blob, followed by an fsync.
   2. The same pages are written in place in `data.db`, followed by an fsync.
   3. `dwb.log` is truncated, and then `wal.log` is truncated.

## Recovery (runs on every open)

1. **Doublewrite repair.** If `dwb.log` is complete and its checksum is valid, the
   previous run crashed during step 3.2 and may have left torn pages behind. All of its
   pages are copied back into `data.db`. If the doublewrite buffer is incomplete, the
   crash happened before any in-place write, so `data.db` is still consistent and the
   buffer is discarded.
2. **Load the meta page**, after verifying its checksum.
3. **WAL replay.** Records are read in order and applied if their LSN is greater than the
   meta page's `checkpoint_lsn`. Replay stops at the first record that is short or fails
   its CRC. That is a torn tail from a crash mid-append, which was never acknowledged,
   and the file is truncated there.
4. A checkpoint makes the recovered state durable.

Records are logical (put/delete of a key), so replaying them on top of the last
checkpoint is idempotent.

## B+Tree

- Keys can be up to 32 bytes and values up to 200 bytes. Node capacity is derived from
  the page size: up to **17 entries per leaf** and **107 separator keys per internal
  node**, so a 4-level tree addresses millions of keys.
- **Insert:** a full leaf splits in half and its right half's first key is *copied* up.
  A full internal node splits and its middle key is *moved* up. When the root splits,
  the tree grows by one level.
- **Delete:** an underfull node first **borrows** from a sibling, rotating through the
  parent separator. If neither sibling can spare a key, the node **merges** with one,
  and the freed page goes on the **free list** for reuse. When the root runs out of
  keys, the tree shrinks by one level.
- **Range scan:** descend once to the start key, then follow the leaf `next` links.
- **Read fast path:** `get` and the descent for insert, delete and scan compare keys
  directly against the serialized page bytes. A page is decoded into a `Node` only when
  it has to change.
- `checkIntegrity()` verifies sorted keys, separator bounds, min/max occupancy, equal
  leaf depth, the leaf chain, the key count, and that every page is accounted for (in
  the tree or on the free list, with no leaks).

## Crash testing

`crash_test` re-runs itself as a child process with one **failpoint** armed. The child
performs 800 deterministic puts and deletes and logs each acknowledged operation. The
failpoint calls `_Exit()` at an exact moment:

| Failpoint | What it simulates |
|-----------|-------------------|
| `wal.torn` | Half a WAL record written, then a crash |
| `db.after_wal` | Record durable but not yet applied to the tree |
| `ckpt.after_dwb` | Doublewrite done, data file not yet touched |
| `ckpt.torn_page` | Half a page written in place (a torn page) |
| `ckpt.after_data` | Data file synced, doublewrite buffer not yet cleared |
| `ckpt.before_wal_reset` | Checkpoint done, WAL not yet truncated |

The parent then reopens the database and requires two things. The integrity check must
pass, and the contents must equal the model state after every acknowledged operation
(plus, optionally, the one in-flight operation). All 14 scenarios pass.

## Performance (MinGW g++ 6.3 -O2, Windows 11, 50k keys, 100-byte values)

| Workload | ops/s |
|---|---|
| random get | ~110,000 |
| range scan, 100 keys each | ~10,000 scans |
| random put, no fsync per op, with checkpoints | ~21,000 |
| random put, no fsync per op, checkpoints deferred | ~42,000 |
| durable put, fsync per write | ~800–1,100 |

The doublewrite buffer doubles checkpoint I/O. That is the cost of being safe against
torn pages, and it can be tuned with `Options::checkpoint_dirty_pages`.

## Possible extensions

- Slotted pages with variable-length records and overflow pages for large values
- Group commit (batching many writers into one fsync)
- Concurrency: a reader-writer latch, or latch crabbing on tree nodes
- Snapshot iterators (MVCC) and prefix compression in internal nodes
