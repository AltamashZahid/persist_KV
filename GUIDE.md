# PersistKV: Concepts and Design Guide

This guide explains every idea PersistKV is built on, from first principles up to the
exact code. Read it top to bottom once, then use it as a reference. Each section points
to the file that implements it.

**Contents**

1. [What a storage engine is](#1-what-a-storage-engine-is)
2. [Why durability is hard: disks, the OS page cache and fsync](#2-why-durability-is-hard)
3. [Pages: how data is laid out on disk](#3-pages)
4. [Choosing an index: BST vs hash vs B-tree vs B+Tree vs LSM](#4-choosing-an-index)
5. [B+Tree operations with variable-size nodes](#5-btree-operations)
6. [Overflow pages for large values](#6-overflow-pages)
7. [The buffer pool (page cache) and the free list](#7-the-buffer-pool-and-free-list)
8. [Write-ahead logging, batches and group commit](#8-write-ahead-logging)
9. [Checksums (CRC32)](#9-checksums)
10. [Checkpoints, the doublewrite buffer, and checkpointing in the background](#10-checkpoints)
11. [Recovery, and every crash case](#11-recovery)
12. [Steal / force: the theory behind the design](#12-steal--force)
13. [Concurrency: locks, lock order and what each lock protects](#13-concurrency)
14. [How it is tested](#14-testing)
15. [Complexity and capacity](#15-complexity-and-capacity)
16. [Limitations](#16-limitations)
17. [Interview questions and answers](#17-interview-questions)
18. [Code reading order](#18-code-reading-order)
19. [Further reading](#19-further-reading)

---

## 1. What a storage engine is

A **storage engine** is the bottom layer of a database. It stores bytes on disk and
finds them again quickly, and it does not lose them. Everything above it (SQL, query
planning, networking) is built on its small API:

```cpp
db.put("alice", "engineer");      // insert or overwrite
db.get("alice", &value);          // point lookup
db.remove("alice");               // delete
db.scan("a", "m", callback);      // ordered range query
db.write(batch);                  // several changes, all-or-nothing
```

Real engines with the same job:

| Engine | Used in | Index structure |
|---|---|---|
| **InnoDB** | MySQL | B+Tree, with a WAL ("redo log"), a doublewrite buffer and group commit, the same design as PersistKV |
| **SQLite** | phones, browsers | B-tree with overflow pages, plus a rollback journal or WAL |
| **LMDB** | OpenLDAP | Copy-on-write B+Tree |
| **RocksDB / LevelDB** | many systems at Meta and Google | LSM tree (PersistKV's group commit is modeled on LevelDB's) |
| **WiredTiger** | MongoDB | B-tree |

An engine has to deliver four properties at once:

1. **Fast lookups**, which comes from the index (B+Tree).
2. **Durability**: once a write returns, it survives a crash. This comes from the WAL.
3. **Integrity**: the engine never silently returns corrupted data. This comes from checksums.
4. **Concurrency**: many threads can use it at once safely. This comes from locks.

---

## 2. Why durability is hard

### Where a write actually goes

```
your program ──write()──► OS page cache (RAM) ──(later, whenever the OS wants)──► disk
                                    │
                               fsync() forces this step now
```

When `write()` returns, the data is **only in RAM**, inside the operating system's page
cache. The OS flushes it to disk whenever it chooses. So:

| Event | Data written but not fsynced | Data that was fsynced |
|---|---|---|
| Your process crashes or is killed | **Survives** (the OS still holds it) | Survives |
| Power loss or kernel panic | **Lost** | Survives |

`fsync(fd)` (POSIX), or `_commit(fd)` on Windows, blocks until the file's data is on
stable storage. PersistKV wraps this in `File::sync()` ([src/file.cpp](src/file.cpp)).

### Torn writes
A disk only guarantees that one **sector** (512 B or 4 KB) is written atomically. If
power fails while a 4 KB page is being written, the page can end up **half old and half
new**. This is a *torn page*. A torn record at the end of a log is called a *torn tail*.

PersistKV handles the two cases separately:
- **Torn WAL records:** a CRC detects them, and the log is truncated at that point (§8).
- **Torn data pages:** the doublewrite buffer repairs them (§10).

### fsync is slow
An fsync costs roughly 0.1–10 ms. A loop of durable writes therefore manages only about
1,000 writes/s. The fixes are **batching** (many changes in one record) and **group
commit** (many threads share one fsync). Both are in §8.

---

## 3. Pages

The data file `data.db` is an array of **4096-byte pages**. The page with id `N` lives
at byte offset `N × 4096`. Page 0 is the **meta page**, which holds the root page id,
the page count, the free-list head, the checkpoint LSN and the key count.

Every page starts with a 16-byte header, from [include/persistkv/pager.h](include/persistkv/pager.h):

```
offset 0   4 bytes  CRC32 of bytes [4, 4096)       ← detects corruption
offset 4   1 byte   type: 1=Meta 2=Leaf 3=Internal 4=Free 5=Overflow
offset 6   2 bytes  number of keys (tree pages) / bytes used (overflow pages)
offset 8   4 bytes  next page: leaf right sibling / overflow chain / free list
```

Page bodies, from [src/btree.cpp](src/btree.cpp):

```
Leaf:      [klen u16][key][cell] [klen u16][key][cell] ...
             cell = [0][vlen u16][value bytes]                 (inline, value ≤ 512 B)
                  | [1][vlen u32][first overflow page id]      (large value, 9 bytes)
Internal:  [child0][child1]...[childN]  [klen][key0] ... [klen][keyN-1]
Overflow:  header, then up to 4080 bytes of the value
```

**Serialization** converts a C++ `Node` into these bytes (`store()`), and
**deserialization** goes back from bytes to a `Node` (`decode()`). Integers are stored
little-endian with `memcpy` (`put32`/`get32` in [common.h](include/persistkv/common.h)).

**Page 0 never belongs to the tree.** That frees the value `0` to mean "no page"
(`kInvalidPage`), the same way `nullptr` works for pointers.

---

## 4. Choosing an index

| Structure | Point lookup | Range scan | Disk friendliness |
|---|---|---|---|
| Sorted array | O(log n) | great | inserts cost O(n) shifts |
| Binary search tree | O(log n) | ok | **bad**: about 20 levels for 1M keys means about 20 random disk reads |
| Hash table | O(1) | **impossible** (no order) | ok |
| **B+Tree** | O(log n) | great | **great**: wide nodes give 3–4 levels for millions of keys |
| LSM tree | O(log n), multiple files | ok | great for writes, costlier reads |

### Fanout is the key idea
On disk, the cost that matters is **pages read**, not comparisons. A B+Tree packs
**many keys into one node** (one page), so the tree is short and wide. PersistKV sizes
nodes **in bytes** ([btree.h](include/persistkv/btree.h)):

- A node is **full** when its body exceeds `kNodeCapacity` = 4080 bytes.
- A non-root node is **underfull** below `kNodeMinFill` = 1020 bytes (25%).

So fanout depends on entry size. A leaf entry is `2 + key + cell`. With a 14-byte key
and a 100-byte value, that's about 120 bytes, so about 30 per leaf. With tiny values it
reaches 150+. An internal entry is `2 + key + 4`, so with 14-byte keys a node holds
about **200 children**. A 3-level tree with 200 children per internal node and 30
entries per leaf holds 200 × 200 × 30 ≈ **1.2 M keys**.

*Why bytes and not a fixed count?* The first version capped every leaf at 17 entries,
sized for the worst-case entry. With typical small values, about 90% of each page was
empty. Byte-based sizing halved the file size for the benchmark data.

### B-tree vs B+Tree
| | B-tree | **B+Tree** (ours) |
|---|---|---|
| Where values live | in every node | **only in leaves** |
| Internal nodes | keys + values + children | keys + children only, so more fanout |
| Range scan | in-order walk, jumping up and down | **descend once, then follow leaf `next` links** |

```
                 [ 30 | 60 ]                    ← internal (routing only)
               /      |      \
   [10 20] ──► [30 40 50] ──► [60 70]           ← leaves, linked left→right
```

**Routing rule:** child `i` holds every key in `[key(i-1), key(i))`. To find a key, take
the first separator **strictly greater** than it (an upper bound), as in
`rawChildFor()`.

---

## 5. B+Tree operations

All code is in [src/btree.cpp](src/btree.cpp).

### 5.1 Search (`get`)
Descend from the root using the routing rule until you reach a leaf, then look for the
key there. That costs O(height) page reads.

*Optimization:* `get` and every descent work **directly on the raw page bytes**
(`rawChildFor`, `rawLeafFind`) and never build a `Node`. Building one allocates a string
per key, and avoiding that made gets about 3× faster.

### 5.2 Insert, delete and update: one unified path
Every change goes through `apply()` → `modify()` → `fixChild()`:

1. `modify()` descends to the leaf and changes it **in memory** (insert, overwrite or
   erase). It returns the modified `Node` to its parent **without storing it**.
2. The parent calls `fixChild()` on that child:
   - **Over a page** (`> 4080 B`): **split** it into two halves of about equal *bytes*,
     and insert the separator into the parent.
   - **Under 25%** (`< 1020 B`): **rebalance** it with a sibling (§5.3).
   - Otherwise: store it.
3. That fix may have changed the parent, so the parent returns itself to *its* parent,
   which repeats the same check. At the top, `apply()` grows the tree if the root
   split, and shrinks it if the root was left with no keys.

**Why one path for everything?** With variable-length keys, *any* change can push a
node in *either* direction:
- Overwriting a value with a shorter one can make a leaf **underfull** without any
  delete.
- Rebalancing replaces a separator in the parent. If the new separator is a longer key,
  the parent can **overflow** after a delete.

With a fixed entry count neither can happen. With byte sizing both can, so every level
checks both conditions.

### 5.3 Rebalancing: merge or redistribute
When a child underflows, take one sibling (the left one if it exists) and concatenate
the two:
- **If the result fits in one page → merge.** Keep the left page, free the right one,
  and remove the separator from the parent.
- **Otherwise → redistribute.** Split the combined entries evenly by bytes back into
  the two pages, and put the new middle key in the parent.

For internal nodes, concatenating means *pulling the parent's separator down* between
the two halves. Redistributing then *pushes a new separator up*. That is a rotation
through the parent.

### 5.4 Why splits and merges always work
`btree.h` asserts that the largest possible entry (645 bytes for a leaf) is at most
**half a page**. From that:
- A node that just overflowed holds at most one page plus one entry, so the balanced
  halves are each at most about 2.7 KB. **Both fit.**
- Redistribution only happens when the pair is over a page, so each half is at least
  (4080 − 645)/2 ≈ 1.7 KB, comfortably above the 1020-byte minimum. **Neither is
  underfull.**
- A merge result contains a sibling that already met the minimum. **Not underfull.**

### 5.5 Copy-up vs push-up (a classic interview question)
- A **leaf** split **copies** the right half's first key up. The key must stay in the
  leaf, because its value lives there.
- An **internal** split **pushes** the middle key up and keeps it in neither half,
  because internal keys are only signposts.

### 5.6 Range scan (`scan`)
Descend once to the first key ≥ `lo`, then follow the leaf `next` links until a key
exceeds `hi` or the callback returns `false`. That costs O(log n + k).

### 5.7 The invariants (`check()`)
1. Keys in every node are strictly sorted.
2. Every key in child `i` is in `[separator(i-1), separator(i))`.
3. Every non-root node uses at least 1020 bytes; every node fits in a page.
4. All leaves are at the same depth.
5. The leaf chain visits every leaf once, in key order.
6. `meta.key_count` equals the number of keys in the tree.
7. Every overflow chain's total length equals its value's recorded size.
8. Every page is exactly one of: the meta page, a tree page, an overflow page, or a
   free page. Nothing leaks, and nothing is shared.

---

## 6. Overflow pages

A value larger than 512 bytes would eat too much of a leaf, and anything over about
4 KB could not fit at all. So `makeCell()` writes it into a **linked chain of overflow
pages**, 4080 bytes each, and the leaf keeps only a 9-byte reference:
`[1][length][first page]`.

```
leaf: ... ["photo:7" → [1][100000][page 812]] ...
                                      │
            [812: 4080 B] → [813: 4080 B] → ... → [836: 2080 B] → 0
```

- **Write:** the chain is written back to front, so each page already knows its
  successor's id.
- **Read** (`readCell`): follow `next` pointers and concatenate.
- **Overwrite or delete** (`freeCell`): walk the chain and put every page on the free
  list.
- Keeping big values out of the leaves keeps fanout high: a leaf holding references is
  as dense as a leaf holding small values.

SQLite does exactly this ("overflow pages"). PostgreSQL's equivalent is called TOAST.

---

## 7. The buffer pool and free list

[src/pager.cpp](src/pager.cpp)

The cache maps `PageId → Frame { 4096 bytes, dirty flag, pin count, LRU position }`.

- **Read:** a hit returns the cached copy and moves it to the front of the LRU list. A
  miss reads the page from disk, **verifies its CRC**, and caches it.
- **Write:** modify the cached copy and mark it **dirty**.
- **Eviction:** drop the least recently used page that is **clean and unpinned**. Dirty
  pages are never evicted; they wait for a checkpoint (§12). Pinned pages are being
  written by a running checkpoint (§10).
- **Thread safety:** one mutex (`cache_mu_`) guards the frame table, so any number of
  readers can use the cache at once.

**Free list.** Freed pages (from merges or overflow chains) are linked through their own
`next` field, starting from `meta.free_head`. `allocate()` pops from the list and only
grows the file when the list is empty. The tests check that delete-everything followed
by re-insert does not grow the file, and the integrity check proves no page is leaked.

---

## 8. Write-ahead logging

[src/wal.cpp](src/wal.cpp), [src/db.cpp](src/db.cpp)

### The WAL rule
> Before a change is applied anywhere, a description of it must be **durably** in the log.

### Record format: one record per batch
```
[crc32][len][lsn u64][count u32]  then count × [op u8][klen u16][vlen u32][key][value]
          └────────────────── the CRC covers everything from here ─────────────────┘
```
A single `put` is a batch of one. Because a whole batch is **one record with one CRC**,
recovery either applies all of it or (if it was torn) none of it. That makes
`WriteBatch` **atomic**, which is what you need for something like a money transfer
that updates two accounts.

### LSN (log sequence number)
Every record gets an increasing LSN. The meta page stores `checkpoint_lsn`, the highest
LSN already reflected in the data file. Recovery skips anything at or below it.

### Logical logging and idempotency
Records describe *operations* ("PUT alice=x"), not byte changes. Replaying them in
order on a consistent tree always gives the same result, even if some were already
applied. The catch: logical redo needs a **structurally valid tree** to replay onto,
and the doublewrite buffer guarantees one (§10).

### Group commit (`DB::commit`)
fsync is the expensive part, so concurrent writers share it. This is the same scheme
LevelDB uses:

```
writer A ──┐                       ┌── A, B, C all return
writer B ──┼─► queue ─► leader A:  │
writer C ──┘           1. append A, B, C to the WAL
                       2. ONE fsync
                       3. apply A, B, C to the tree
                       4. wake B and C ─────────┘
```

1. Each writer pushes itself onto a queue and sleeps until it is at the front or its
   work is done.
2. The writer at the front is the **leader**. It takes every queued batch (up to
   4 MB), appends them all, **fsyncs once**, and applies them.
3. It marks them done and wakes everyone, and the next writer in line becomes the next
   leader.

The result: 16 threads committed 2,000 durable writes with **236 fsyncs**, 4.3× the
single-thread throughput. There is no loss of safety, because no writer returns before
the fsync covering its batch has finished.

### Torn tails
Replay reads records until one is incomplete or fails its CRC, then truncates the file
there. That record was never acknowledged, so nothing that was promised is lost.

### `sync_writes`
- `true` (default): fsync on every commit. Survives power loss.
- `false`: no fsync per commit. Survives a process crash, not power loss. Much faster.

---

## 9. Checksums

[src/crc32.cpp](src/crc32.cpp)

**CRC32** gives a 32-bit fingerprint of a block of bytes. Flip any bit and it changes,
except with a probability of about 1 in 4 billion.

- **Every WAL record** carries a CRC: it catches torn tails and garbage.
- **Every page** carries one: it catches torn pages, bit rot and bad writes.
- **The doublewrite file** has one over its whole contents: it proves the file is complete.

On a mismatch the engine throws `CorruptionError` instead of returning bad data. A CRC
detects *accidental* damage only. It is not cryptographic, so it can't detect deliberate
tampering.

---

## 10. Checkpoints

[src/pager.cpp](src/pager.cpp) (`beginCheckpoint` / `writeCheckpoint` / `endCheckpoint`),
[src/db.cpp](src/db.cpp) (`runCheckpoint`)

### Why checkpoint?
Without checkpoints the WAL grows forever and recovery replays all of history. A
checkpoint writes the dirty pages into the data file, after which the log can be
emptied. One is triggered when the WAL reaches 8 MB or 2,048 pages are dirty.

### The torn-page problem, and the doublewrite buffer
A checkpoint writes many pages. If power fails midway, the data file is a mix of old
and new pages, and one page may be torn. Logical WAL replay can't repair a broken tree.
The fix is to **write everything twice**:

```
1. Write ALL pages + meta into dwb.log as one blob with one CRC.  fsync.
   ── crash? dwb.log fails its CRC → discard it. data.db was never touched. ✔
2. Write the same pages in place in data.db.  fsync.
   ── crash? data.db may be torn, but dwb.log is complete → copy it back. ✔
3. Truncate dwb.log.
```
At every moment one complete, consistent copy exists: the old checkpoint in `data.db`,
or the new one in `dwb.log`.

### Doing it in the background
A checkpoint writes up to about 16 MB and fsyncs twice. Done in a writer's thread, that
stalls the writer. PersistKV splits it into three phases and runs it on its own thread:

```
                     ┌─ holds WAL lock + tree lock (milliseconds) ──────────────┐
1. beginCheckpoint:  │ copy dirty pages + meta into a snapshot                  │
                     │ mark them clean, PIN them in the cache                   │
                     │ fsync the active WAL; switch appends to the other WAL    │
                     └──────────────────────────────────────────────────────────┘
2. writeCheckpoint:  doublewrite + in-place writes + fsyncs   ← NO tree lock held:
                                                               reads & writes continue
3. finish:           empty the retired WAL; unpin the pages
```

Two details make this correct:
- **Two WAL files.** Records up to the snapshot LSN are in the retired file. Newer
  commits go to the other file while the checkpoint runs. The retired file is emptied
  only *after* the checkpoint is durable, so a crash at any moment still has every
  record that isn't in `data.db`.
- **Pinning.** After the snapshot, the flushed pages are marked clean. Without a pin
  they could be evicted, and a reader would re-load the **old** version from `data.db`
  before the checkpoint had written the new one. A pin keeps them in memory until
  `data.db` holds them.

**Backpressure:** if dirty pages pile up to 4× the threshold faster than checkpoints can
drain them, writers wait. Memory stays bounded.

### How other engines solve torn pages
| Engine | Technique |
|---|---|
| InnoDB (MySQL) | **Doublewrite buffer**, as here |
| PostgreSQL | **Full-page writes**: the first change to a page after a checkpoint logs the whole page in the WAL |
| LMDB, btrfs | **Copy-on-write**: never overwrite; write new pages, then switch the root |
| SQLite (rollback mode) | **Rollback journal**: save the old pages first, restore them on crash |

---

## 11. Recovery

The `DB::DB()` constructor in [src/db.cpp](src/db.cpp):

```
1. Pager::recoverDoublewrite()
     dwb.log valid (CRC ok)?  → copy every page into data.db, fsync, truncate it
     dwb.log invalid?         → crash was before any in-place write; discard it
2. Load the meta page (verify CRC and magic number)
3. Read both WAL files; truncate any torn tail
   sort all records by LSN; apply those with lsn > checkpoint_lsn
4. checkpoint, then empty both WAL files
```

### Every crash point, and what happens
These are exactly the scenarios [tests/crash_test.cpp](tests/crash_test.cpp) triggers:

| Crash happens… | State on disk | Recovery result |
|---|---|---|
| mid WAL append (`wal.torn`) | half a record at the end of the log | CRC fails → truncated. That commit was never acknowledged. |
| after the WAL fsync, before applying (`db.after_wal`) | record durable | Replayed: the commit appears (allowed, it was in flight) |
| after the doublewrite fsync (`ckpt.after_dwb`) | dwb complete, data.db old | dwb copied in |
| mid in-place write (`ckpt.torn_page`) | a **torn page** | dwb copy overwrites it |
| after the data fsync (`ckpt.after_data`) | data.db new, dwb present | dwb re-applied (same bytes, harmless) |
| before the WAL reset (`ckpt.before_wal_reset`) | data.db new, retired log not emptied | its records are ≤ checkpoint_lsn → skipped |
| **during recovery itself** | any of the above, partly repaired | Recovery is idempotent, so it simply runs again |

### Relation to ARIES
ARIES (DB2, SQL Server) has three phases: Analysis, Redo and Undo. PersistKV needs only
**Redo**, because the data file never contains a partial or uncommitted change (§12).

---

## 12. Steal / force

Two questions define every buffer manager:
- **Steal?** Can a dirty page reach disk *before* its change is committed?
- **Force?** Must dirty pages reach disk *at* commit?

| | Force | **No-force** |
|---|---|---|
| **Steal** | needs undo | needs undo + redo (ARIES, most databases) |
| **No-steal** | needs nothing, but slow | **redo only (PersistKV)** |

- **No-steal:** dirty pages are never evicted; they only reach disk inside a
  checkpoint, and a snapshot is taken only between commits. So the data file never
  holds a half-applied change, and **no undo log is needed**.
- **No-force:** commits write only the small WAL record, and the WAL provides **redo**.

The cost: dirty pages must fit in RAM between checkpoints. Backpressure (§10) enforces
that.

---

## 13. Concurrency

[src/db.cpp](src/db.cpp), [include/persistkv/sync.h](include/persistkv/sync.h)

### The locks
| Lock | Type | Protects | Held by |
|---|---|---|---|
| `tree_mu_` | reader-writer | the B+Tree pages and the meta page | `get`/`scan`: **shared** (many at once). Applying a commit and taking a checkpoint snapshot: **exclusive**, in memory only |
| `wal_mu_` | mutex | the active WAL, LSN counter, log rotation | the group-commit leader (append + fsync + apply), and the checkpoint snapshot |
| `commit_mu_` + condvar | mutex | the writer queue | every writer, briefly |
| `ckpt_run_mu_` | mutex | "one checkpoint at a time" | `runCheckpoint` |
| `cache_mu_` | mutex | the page cache's frame table | every page read and write |

### Lock order
`ckpt_run_mu_ → wal_mu_ → tree_mu_ → cache_mu_`. Every code path acquires locks in this
order. A deadlock needs two threads each holding a lock the other wants, and with a
single global order that cycle can't form.

### What runs in parallel
- **Readers with readers:** always (shared lock).
- **Readers with a writer's fsync:** yes. The leader holds only `wal_mu_` during the
  fsync; it takes the tree lock exclusively only for the in-memory apply.
- **Readers and writers with a checkpoint's disk I/O:** yes. The checkpoint holds no
  tree lock while writing.
- **Writer with writer:** they don't run side by side, but they *share* fsyncs (group
  commit).

### Portable threads
The MinGW toolchain used here has no `std::thread`/`std::mutex`. So `sync.h` wraps
**Windows SRW locks** (one pointer-sized lock with shared and exclusive modes) and
**condition variables** directly, and uses the C++ standard library on Linux and macOS.

---

## 14. Testing

| Kind | How |
|---|---|
| **Model-based** | Run random operations on PersistKV *and* `std::map`, then compare. Checked with `checkIntegrity()` throughout, with a tiny cache to force evictions |
| **Boundary** | Value sizes at 0, 512, 513, 4080, 4081 bytes … up to 1 MiB |
| **Path coverage** | The variable-size test has a grow phase *and* a shrink phase, so it really exercises merges, redistribution and root collapse |
| **Fault injection** | Named failpoints compiled into the engine. A child process is killed (`_Exit`) at an exact point, including mid-record and mid-page torn writes. The parent recovers and compares against the acknowledged operations |
| **Crash fuzzing** | Random workloads (single ops, batches, overflow values) crashed at a random failpoint hit. Half are crashed again *during recovery*. 500 runs on every CI push |
| **Concurrency** | 4 writers + 4 readers while checkpoints run constantly. Every value embeds its key, so a reader can detect a torn or mixed read |
| **Sanitizers** | CI runs AddressSanitizer + UBSan (memory errors, undefined behavior) and ThreadSanitizer (data races) |

**An honest caveat:** killing a process does not discard the OS page cache, so the crash
tests prove the *logic* (ordering, torn-write handling, replay), not physical power
loss. Testing real power loss needs tools like `dm-log-writes`, or pulling the plug on
a VM.

**Bugs these tests caught while building this project:**
- MSVC's `atol` clamps values above 2³¹, so a crash-test child process ran a different
  workload than the one its parent verified.
- The MinGW C runtime expands `*` in command-line arguments, which broke the child's
  arguments and made it spawn children of its own: a fork bomb.
- A data race on the retired WAL's size, found during the ThreadSanitizer audit.

---

## 15. Complexity and capacity

| Operation | Time |
|---|---|
| get | O(log n) page reads (3–4 levels) |
| put / remove | O(log n), plus O(1) amortized splits or merges |
| scan of k keys | O(log n + k) |
| value of size v | +⌈v / 4080⌉ overflow pages |
| recovery | O(WAL size + doublewrite size) |

Limits: keys 1–128 bytes, values up to 1 MiB, batches up to 64 MiB. Page ids are 32-bit,
which allows 2³² pages × 4 KB = 16 TiB.

---

## 16. Limitations

| Limitation | What a production system does |
|---|---|
| Writers apply one at a time under the tree lock | **Latch crabbing**: per-node latches, released once a child is "safe" |
| A long scan holds the shared lock and delays writers | **MVCC**: readers see a snapshot and never block writers |
| No prefix compression | Store a shared prefix once per node to raise fanout |
| No multi-key transactions with isolation | A transaction manager with locks or MVCC |
| Little-endian only | Explicit byte-order encoding |
| No directory fsync after creating files (Linux) | fsync the directory, so a new file's directory entry is durable |

---

## 17. Interview questions

**Q: Why a B+Tree instead of a hash table?**
Hash tables can't do range queries or ordered iteration. A B+Tree gives O(log n) lookups
*and* ranges through its linked leaves, and its high fanout keeps it 3–4 levels deep.

**Q: Why do your nodes split by bytes instead of by number of keys?**
Entries vary in size. A fixed count must assume the worst case, which wasted about 90%
of each page in my first version. Sizing by bytes halved the file. The cost: a
separator change can push a parent over *or* under its limit, so every level checks
both.

**Q: How do you know a split or merge always produces valid nodes?**
I cap every entry at half a page (a `static_assert`). Then the halves of an overflowed
node each fit, a redistribution only happens when the pair is over one page (so each
half is at least about 1.7 KB, above the 1 KB minimum), and a merge contains a sibling
that already met the minimum.

**Q: How do you store a 1 MB value?**
In a linked chain of overflow pages; the leaf stores a 9-byte reference. Deleting or
overwriting the value frees the chain onto the free list. Leaves stay dense.

**Q: What does "durable" mean here, and how is it guaranteed?**
Once a write returns, it survives a crash. The batch is in the WAL and fsynced before
the call returns, and recovery replays it.

**Q: What is group commit?**
Concurrent writers queue up. The front one, the leader, writes everyone's records with
**one fsync**, applies them, and wakes the others. Fewer fsyncs, the same guarantee:
nobody returns before the fsync covering its data. With 16 threads that was 2,000
commits in 236 fsyncs.

**Q: How is a WriteBatch atomic?**
The whole batch is one WAL record with one CRC. A torn record fails its CRC and is
dropped entirely. An intact one is replayed entirely.

**Q: Why do you need a doublewrite buffer if you have a WAL?**
The WAL is logical. Replaying it needs a valid tree, and a crash mid-checkpoint can
leave torn or mismatched pages. The doublewrite buffer always leaves one complete copy
to restore. (PostgreSQL logs full page images instead.)

**Q: How can a checkpoint run while writes continue?**
It takes the locks only to copy the dirty pages and switch new commits to a second WAL
file. The slow disk writes then happen without the tree lock. The retired WAL is
emptied only after the checkpoint is durable, and the flushed pages are pinned so a
reader can't evict them and reload a stale copy from disk.

**Q: Why don't you need an undo log?**
No-steal: dirty pages reach disk only through checkpoints, and a snapshot is only taken
between commits. The data file never holds a partial change.

**Q: How do you avoid deadlocks?**
There is a single global lock order (checkpoint → WAL → tree → cache), and every path
follows it.

**Q: What happens if a write fails halfway, for example on an I/O error?**
The tree in memory may be half-modified, so the DB enters a failed state: it refuses
all calls and never checkpoints. The WAL still has every committed batch, so reopening
recovers.

**Q: Is recovery safe if you crash *during* recovery?**
Yes. Recovery doesn't change the data file until its own checkpoint, which uses the
same doublewrite protocol, and replay skips records at or below `checkpoint_lsn`. The
fuzzer crashes half its runs a second time during recovery to test exactly this.

**Q: How did you test crash safety?**
Fault injection. Named crash points kill a child process at exact moments, including
torn writes, and a fuzzer crashes random workloads at random points. The parent
recovers the database and checks it against a model of the acknowledged operations,
plus a full invariant check. That's 500 fuzz runs per CI build.

**Q: B+Tree vs LSM tree?**
An LSM tree buffers writes in memory, writes sorted files sequentially and merges them
in the background. That gives very fast writes, but a read may check several files. A
B+Tree updates in place, so reads are faster and more predictable while writes cost
more. RocksDB is an LSM tree; InnoDB is a B+Tree.

**Q: How would you let writers run in parallel?**
Per-node latches with **latch crabbing**: latch a child, and release the ancestors once
the child can't split or merge. Add MVCC so long scans don't block writers.

---

## 18. Code reading order

1. [include/persistkv/common.h](include/persistkv/common.h): constants, errors, byte encoding
2. [src/file.cpp](src/file.cpp), [src/crc32.cpp](src/crc32.cpp): I/O, fsync, checksums
3. [include/persistkv/write_batch.h](include/persistkv/write_batch.h), [src/wal.cpp](src/wal.cpp): the log
4. [src/pager.cpp](src/pager.cpp): page cache, free list, three-phase checkpoint, doublewrite recovery
5. [src/btree.cpp](src/btree.cpp): read paths → `apply`/`modify`/`fixChild` → `splitNode`/`rebalance` → overflow cells → `check`
6. [src/sync.cpp](src/sync.cpp), then [src/db.cpp](src/db.cpp): group commit, checkpoint thread, and recovery in the constructor
7. [tests/crash_test.cpp](tests/crash_test.cpp), [src/failpoint.cpp](src/failpoint.cpp): how crash safety is proven

---

## 19. Further reading

- **CMU 15-445 Database Systems** (free lectures on YouTube). Buffer pools, B+Trees,
  latch crabbing, logging and recovery. The closest match to this project.
- **Alex Petrov, *Database Internals*** (O'Reilly). B-trees, page layouts, overflow
  pages and recovery.
- **Mohan et al., "ARIES"** (1992). The standard recovery algorithm.
- **SQLite: "Atomic Commit In SQLite"**, and its file-format docs (overflow pages).
- **LevelDB source, `DBImpl::Write`**. The group commit design used here.
- **Dan Luu, "Files are hard"**. Why fsync correctness is difficult in practice.
- **MySQL docs: "InnoDB Doublewrite Buffer"**, and **PostgreSQL docs: `full_page_writes`**.
