# PersistKV: Concepts and Design Guide

This guide explains every idea PersistKV is built on, from first principles up to the
exact code. Read it top to bottom once, then use it as a reference. Each section points
to the file that implements it.

**Contents**

1. [What a storage engine is](#1-what-a-storage-engine-is)
2. [Why durability is hard: disks, the OS page cache and fsync](#2-why-durability-is-hard)
3. [Pages: how data is laid out on disk](#3-pages)
4. [Choosing an index: BST vs hash vs B-tree vs B+Tree vs LSM](#4-choosing-an-index)
5. [B+Tree operations, step by step](#5-btree-operations)
6. [The buffer pool (page cache)](#6-the-buffer-pool)
7. [The free list](#7-the-free-list)
8. [Write-ahead logging (WAL)](#8-write-ahead-logging)
9. [Checksums (CRC32)](#9-checksums)
10. [Checkpoints and the doublewrite buffer](#10-checkpoints-and-the-doublewrite-buffer)
11. [Recovery, and every crash case](#11-recovery)
12. [Steal / force: the theory behind the design](#12-steal--force)
13. [How it is tested](#13-testing)
14. [Complexity and capacity](#14-complexity-and-capacity)
15. [Limitations](#15-limitations)
16. [Interview questions and answers](#16-interview-questions)
17. [Code reading order](#17-code-reading-order)
18. [Further reading](#18-further-reading)

---

## 1. What a storage engine is

A **storage engine** is the bottom layer of a database. It stores bytes on disk and
finds them again quickly, and it does not lose them. Everything above it (SQL,
query planning, networking) is built on its small API:

```cpp
db.put("alice", "engineer");      // insert or overwrite
db.get("alice", &value);          // point lookup
db.remove("alice");               // delete
db.scan("a", "m", callback);      // ordered range query
```

Real engines with the same job:

| Engine | Used in | Index structure |
|---|---|---|
| **InnoDB** | MySQL | B+Tree, with a WAL ("redo log") and a doublewrite buffer, the same design as PersistKV |
| **SQLite** | phones, browsers | B-tree, with a rollback journal or WAL |
| **LMDB** | OpenLDAP | Copy-on-write B+Tree |
| **RocksDB / LevelDB** | many systems at Meta and Google | LSM tree |
| **WiredTiger** | MongoDB | B-tree |

An engine has to deliver three properties at once:

1. **Fast lookups**, which comes from the index (B+Tree).
2. **Durability**: once `put` returns, the data survives a crash. This comes from the WAL.
3. **Integrity**: the engine never silently returns corrupted data. This comes from checksums.

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

`fsync(fd)` (POSIX), or `_commit(fd)` / `FlushFileBuffers` on Windows, blocks until the
file's data is on stable storage. PersistKV wraps this in `File::sync()`
([src/file.cpp](src/file.cpp)).

### Torn writes

A disk only guarantees that one **sector** (512 B or 4 KB) is written atomically, and
sometimes not even that. If the power fails while a 4 KB page is being written, the
page can end up **half old and half new**. This is a *torn page*. A torn WAL record at
the end of the log is called a *torn tail*.

This is why PersistKV has two separate safety mechanisms:

- **Torn WAL records:** a CRC detects them, and the log is truncated at that point (§8).
- **Torn data pages:** the doublewrite buffer repairs them (§10).

### fsync is slow

An fsync costs roughly 0.1–10 ms, depending on the drive. That is why the benchmark
shows about 1,000 durable puts/s against about 20,000+ puts/s with fsync disabled. Real
databases reduce the cost with **group commit**, where one fsync covers many
transactions.

---

## 3. Pages

Disks and operating systems work in blocks, so the engine does too. The data file
`data.db` is an array of **4096-byte pages**. The page with id `N` lives at byte offset
`N × 4096`.

```
data.db:  [ page 0: META ][ page 1 ][ page 2 ][ page 3 ] ...
             root=7         leaf      internal   free
             page_count
             free_head
             checkpoint_lsn
             key_count
```

### Page header (every page, 16 bytes), from [include/persistkv/pager.h](include/persistkv/pager.h)

```
offset 0   4 bytes  CRC32 of bytes [4, 4096)  ← detects corruption
offset 4   1 byte   type: 1=Meta 2=Leaf 3=Internal 4=Free
offset 6   2 bytes  number of keys
offset 8   4 bytes  next page id (leaf: right sibling; free page: next free)
```

### Page bodies, from [src/btree.cpp](src/btree.cpp)

```
Leaf:      [klen][key bytes][vlen][value bytes]  [klen][key][vlen][value] ...
Internal:  [child0][child1]...[childN]  [klen][key0] [klen][key1] ... [klen][keyN-1]
```

**Serialization** means converting a C++ object (`Node`, with vectors of strings) into
these bytes (`store()`), and **deserialization** goes from bytes back to an object
(`decode()`).

**Endianness:** integers are stored little-endian with `memcpy` (`put32`/`get32` in
[common.h](include/persistkv/common.h)). That is correct on x86 and ARM. A big-endian
machine would need byte swapping.

**Page 0 never belongs to the tree.** That frees the value `0` to mean "no page"
(`kInvalidPage`), the same way `nullptr` works for pointers.

---

## 4. Choosing an index

| Structure | Point lookup | Range scan | Disk friendliness |
|---|---|---|---|
| Sorted array | O(log n) | great | inserts cost O(n) shifts |
| Binary search tree | O(log n) | ok | **bad**: about 20 levels for 1M keys means about 20 random disk reads |
| Hash table | O(1) | **impossible** (no order) | ok |
| **B-tree / B+Tree** | O(log n) | great | **great**: wide nodes give 3–4 levels for millions of keys |
| LSM tree | O(log n), multiple files | ok | great for writes, costlier reads |

### The key idea: fanout

On disk, the cost that matters is **the number of pages read**, not the number of
comparisons. A B-tree puts **many keys in one node** (one page), so the tree is short
and wide.

PersistKV's capacities, computed from the page size in
[btree.h](include/persistkv/btree.h):

- Leaf: up to **17** entries (min 8). Each entry can be up to 2+32+2+200 = 236 bytes.
- Internal: up to **107** keys and **108** children (min 53 keys).

Maximum keys at each height (all nodes full):

| Height | Max keys |
|---|---|
| 1 | 17 |
| 2 | 17 × 108 ≈ 1.8 K |
| 3 | 17 × 108² ≈ 198 K |
| 4 | 17 × 108³ ≈ 21 M |
| 5 | ≈ 2.3 B |

Real nodes are typically 50–70% full, which is why the 200K-key benchmark showed height
4. In practice the top levels stay cached in RAM, so a lookup costs about one disk read.

### B-tree vs B+Tree

| | B-tree | **B+Tree** (ours) |
|---|---|---|
| Where values live | in every node | **only in leaves** |
| Internal nodes | keys + values + children | keys + children only, so more fanout |
| Range scan | in-order tree walk, jumping up and down | **descend once, then follow leaf `next` links** |

That linked list of leaves is why range queries are efficient:

```
                 [ 30 | 60 ]                    ← internal (routing only)
               /      |      \
   [10 20] ──► [30 40 50] ──► [60 70]           ← leaves, linked left→right
```

`scan("35", "65")` descends to the leaf containing 35, then walks right: 40, 50 → next
leaf → 60, then stops at 70 > 65.

### Routing rule

In an internal node with keys `k0 … kn-1`, child `i` holds every key in
`[k(i-1), k(i))`. To find a key, take the first separator **strictly greater** than it
(`upper_bound`). See `rawChildFor()` in [src/btree.cpp](src/btree.cpp).

---

## 5. B+Tree operations

All code is in [src/btree.cpp](src/btree.cpp).

### 5.1 Search (`get`)
Start at the root and pick a child with the routing rule until you reach a leaf, then
look for the key in the leaf. That costs O(height) = O(log n) page reads.

*Optimization in our code:* `get` never builds a `Node` object. It compares keys
directly against the raw page bytes (`rawChildFor`, `rawLeafFind`). Building a `Node`
allocates about 100 strings for an internal page, and skipping that made gets **3×
faster** (40K → 110K ops/s).

### 5.2 Insert (`put` → `insertInto`)

1. Descend to the correct leaf, recursing so we can return back up.
2. If the key exists, overwrite the value. Done.
3. Otherwise insert it in sorted position. If the leaf now has ≤ 17 entries, done.
4. **Leaf split:** move the upper half into a new leaf, link
   `left.next → right → old next`, and **copy** the right leaf's first key up to the
   parent as a separator.
5. The parent inserts `(separator, new child)`. If that makes the parent overflow, it
   **splits** too, and its middle key is **moved** (pushed) up, not copied.
6. If the **root** splits, create a new root with 2 children. The tree grows **taller
   by 1**. A B+Tree only ever grows at the root, so all leaves stay at the same depth.

```
Insert 25 into a full leaf (max 4 for this example):

before:  parent [ 30 ]           after:   parent [ 20 | 30 ]
                /    \                           /    |     \
        [10 15 20 22] [30..]          [10 15] [20 22 25] [30..]
                                        ↑ copy-up "20"
```

**Copy-up vs push-up** is a classic interview question. A leaf split *copies* the
separator up, because the key must stay in the leaf where its value lives. An internal
split *pushes* the middle key up, because internal keys are only signposts.

### 5.3 Delete (`remove` → `removeFrom` → `rebalance`)

1. Descend and remove the key from its leaf.
2. If the leaf still has ≥ 8 entries (half full), done. The parent separator may now
   name a deleted key. **That is fine**: separators only need to route correctly, not
   exist in the data.
3. If the leaf **underflows**:
   - **Borrow from the left sibling** if it has more than the minimum: move its last
     entry over and update the parent separator.
   - Otherwise **borrow from the right sibling**.
   - Otherwise **merge** with a sibling. Both are at the minimum, so together they fit
     in one node. Remove the separator from the parent, and put the emptied page on the
     **free list**.
4. A merge removes a key from the parent, so the parent can underflow too. The same
   fix repeats one level up. For internal nodes, a borrow is a **rotation through the
   parent**: the parent's separator comes down and the sibling's key goes up.
5. If the root ends up as an internal node with 0 keys, its single child becomes the
   new root. The tree gets **shorter by 1**.

```
Borrow (leaf, min 2 for this example), deleting 40:

parent [ 30 | 50 ]                     parent [ 30 | 60 ]
  [10 20] [30 40] [50 60 70]   ──►       [10 20] [30 50] [60 70]
          underflow ↑   lends 50
```

### 5.4 Range scan (`scan`)
Descend once to the first key ≥ `lo`, then iterate leaves through `next` until a key
exceeds `hi` or the callback returns `false`. That costs O(log n + k) for k results.

### 5.5 The invariants (`check()`)
A correct B+Tree must always satisfy all of these, and the tests verify every one:

1. Keys in every node are strictly sorted.
2. Every key in child `i` lies in `[separator(i-1), separator(i))`.
3. Every non-root node is at least half full; every node is at most full.
4. All leaves are at the same depth.
5. The leaf `next` chain visits every leaf once, in key order.
6. The number of keys in the tree equals `meta.key_count`.
7. Every page is either the meta page, in the tree, or on the free list. Nothing leaks.

---

## 6. The buffer pool

Implemented in [src/pager.cpp](src/pager.cpp).

Reading from disk is slow, so recently used pages are kept in RAM in a **frame** table:

```
unordered_map<PageId, Frame>     Frame = { 4096 bytes, dirty flag, LRU position }
list<PageId> lru_                front = most recently used, back = least
```

- **Read:** a hit returns the cached copy and moves it to the front of the LRU list. A
  miss reads it from disk, **verifies the checksum**, and caches it.
- **Write:** modify the cached copy and mark it **dirty**.
- **Eviction (LRU):** when the cache is over capacity, drop the least recently used
  **clean** page. **Dirty pages are never evicted.** They stay in memory until the next
  checkpoint (see §12 for why).
- `stats` reports `cache_hits` and `cache_misses`, so you can see the hit rate.

---

## 7. The free list

A merge frees a page. Instead of leaving a hole in the file, PersistKV threads freed
pages into a **linked list** stored inside the pages themselves:

```
meta.free_head ──► [page 9: FREE, next=4] ──► [page 4: FREE, next=0]
```

- `free(id)` writes a FREE page whose `next` is the old head, then makes it the new head.
- `allocate()` pops the head if the list isn't empty, otherwise appends at the end of
  the file (`page_count++`).

The test `grow_then_shrink_to_empty_and_reuse_pages` checks that inserting, deleting
everything, and inserting again **does not grow the file**.

---

## 8. Write-ahead logging

Implemented in [src/wal.cpp](src/wal.cpp) and [src/db.cpp](src/db.cpp).

### The WAL rule
> Before a change is applied anywhere, a description of it must be **durably** written
> to the log.

`DB::put`:
```
1. record = {lsn = last_lsn + 1, PUT, key, value}
2. append record to wal.log, then fsync   ← from here on the write is durable
3. apply to the B+Tree (in RAM)
4. return to the caller                   ← the write is "acknowledged"
```

If the process crashes after step 2, recovery re-applies the record from the log. If
it crashes before step 2 finishes, the caller never got an acknowledgement, so losing
the write is allowed.

### Why the log is fast
Appending to the end of one file is **sequential I/O**, the fastest thing a disk does.
Updating B+Tree pages in place would mean random writes all over the file. The WAL lets
us do the cheap sequential write on every operation and the expensive random writes
rarely, in batches, at checkpoints.

### Record format
```
[crc32 u32][len u32][lsn u64][op u8][klen u16][vlen u16][key bytes][value bytes]
             └─────────────── the CRC covers everything from here ───────────────┘
```

### LSN (log sequence number)
Every record gets an increasing number. The meta page stores `checkpoint_lsn`, the
highest LSN whose effect is already in the data file. During recovery, records with
`lsn <= checkpoint_lsn` are skipped.

### Logical vs physical logging
- **Physical** logging records byte changes: "page 7, offset 120, write these bytes".
- **Logical** logging records operations: "PUT alice=engineer". **That is ours.**
- **Physiological** logging (ARIES, InnoDB) records a logical change within a page:
  "page 7: insert alice".

Logical records are small and simple. They are **idempotent** when replayed in order
on a consistent state: applying "PUT alice=x" twice gives the same result as applying
it once. The catch is that logical redo **needs the data file to be structurally
consistent** before replay. That is exactly what the doublewrite buffer guarantees (§10).

### Torn tail handling
On replay, records are read until one is incomplete or fails its CRC. Everything from
that point on is a torn tail from a crash mid-append. That record was never
acknowledged, so the file is **truncated** there and nothing acknowledged is lost.

### `sync_writes` option
- `true` (default): fsync every write. Survives power loss. About 1K writes/s.
- `false`: no fsync per write. Survives a **process** crash (the OS keeps the data) but
  not power loss. About 20K+ writes/s.

---

## 9. Checksums

Implemented in [src/crc32.cpp](src/crc32.cpp).

**CRC32** turns a block of bytes into a 32-bit fingerprint. Change any bit and the
fingerprint (almost certainly) changes.

- **Every WAL record** has a CRC, which catches torn tails and garbage.
- **Every page** has a CRC in bytes 0–3, which catches torn pages, bit rot, and bugs
  that write the wrong bytes.
- **The doublewrite file** has a CRC over its entire contents, which tells recovery
  whether that file is complete.

How it works: the algorithm treats the data as one huge binary number and divides it by
a fixed polynomial (`0xEDB88320`, reflected IEEE). The remainder is the CRC. The
table-driven version processes one byte per step using a precomputed 256-entry table.
The test checks the standard vector: `crc32("123456789") == 0xCBF43926`.

What CRC does **not** protect against: deliberate tampering, because anyone can
recompute it. Protecting against that needs a cryptographic hash or MAC. CRC is for
*accidental* corruption.

On a checksum mismatch, PersistKV throws `CorruptionError` **instead of returning bad
data**. The test `corrupted_page_is_detected` flips one byte on disk and expects that
exception.

---

## 10. Checkpoints and the doublewrite buffer

Implemented in `Pager::checkpoint()` in [src/pager.cpp](src/pager.cpp).

### Why checkpoint at all?
Without checkpoints the WAL would grow forever and recovery would replay all of
history. A checkpoint writes the dirty pages into the data file, after which the log
can be emptied.

The trigger is in `DB::maybeCheckpoint`: the WAL reaches **8 MB**, or **2048 dirty
pages** accumulate. A checkpoint also runs on clean shutdown (the destructor) and after
recovery.

### The problem: torn pages during a checkpoint
A checkpoint writes many pages. If power fails in the middle:
- some pages are new and some are old, which is **structurally inconsistent** (a parent
  might point to a child that was never written), and
- one page may be **torn**, half old and half new.

Logical WAL replay can't fix that, because "PUT alice" cannot be applied to a broken
tree.

### The solution: write everything twice
```
1. Write ALL dirty pages + the meta page into dwb.log as one blob with one CRC.  fsync.
   ── crash here? dwb.log is incomplete (bad CRC). data.db was never touched.
                  Recovery throws dwb.log away; data.db = previous checkpoint. ✔

2. Write the same pages in place in data.db.  fsync.
   ── crash here? data.db may be torn or inconsistent, but dwb.log is complete.
                  Recovery copies all of dwb.log back into data.db. ✔

3. Truncate dwb.log, then truncate wal.log.
```

At every moment there is one complete, consistent copy of the database: either the old
checkpoint (data.db) or the new one (dwb.log).

### How other engines solve the same problem
| Engine | Technique |
|---|---|
| InnoDB (MySQL) | **Doublewrite buffer**, the same approach as ours |
| PostgreSQL | **Full-page writes**: the first change to a page after a checkpoint logs the whole page into the WAL |
| LMDB, btrfs | **Copy-on-write / shadow paging**: never overwrite a page; write new copies, then atomically switch the root pointer |
| SQLite (rollback mode) | **Rollback journal**: save the *old* pages before overwriting, and restore them on crash |

---

## 11. Recovery

Implemented in the `DB::DB()` constructor in [src/db.cpp](src/db.cpp). It runs on every open:

```
1. Pager::recoverDoublewrite()
     dwb.log empty?           → nothing to do
     dwb.log valid (CRC ok)?  → copy every page into data.db, fsync, truncate dwb.log
     dwb.log invalid?         → crash happened before in-place writes; discard it
2. Load the meta page (verify its CRC and magic number)
3. Wal::replay()
     for each record with a valid CRC, in order:
        if lsn > checkpoint_lsn: apply it to the tree
     stop at the first torn or corrupt record and truncate the file there
4. checkpoint() so the recovered state is on disk and the WAL starts empty
```

### Every crash point, and what happens

These are the exact scenarios [tests/crash_test.cpp](tests/crash_test.cpp) triggers:

| Crash happens… | State on disk | Recovery result |
|---|---|---|
| mid WAL append (`wal.torn`) | half a record at the end of the WAL | CRC fails → truncated. That op was never acknowledged. |
| after the WAL fsync, before the tree update (`db.after_wal`) | record durable, tree not updated | Record replayed. The op appears, which is allowed because it was in flight. |
| during the doublewrite write | dwb.log incomplete | dwb discarded, full WAL replayed on top of the old checkpoint |
| after the dwb fsync (`ckpt.after_dwb`) | dwb complete, data.db old | dwb copied in; WAL records ≤ checkpoint_lsn skipped |
| mid in-place write (`ckpt.torn_page`) | **a torn page** in data.db | dwb copy overwrites the torn page |
| after the data fsync (`ckpt.after_data`) | data.db new, dwb still present | dwb re-applied (harmless, same bytes) |
| before the WAL reset (`ckpt.before_wal_reset`) | data.db new, old WAL still present | all WAL records ≤ checkpoint_lsn → skipped |

### Relation to ARIES
**ARIES** is the classic recovery algorithm used by DB2, SQL Server and others. It has
three phases: **Analysis** (what was in flight?), **Redo** (repeat history) and **Undo**
(roll back uncommitted changes). PersistKV only needs **redo**. It never writes
uncommitted or partial changes to the data file, so there is nothing to undo (§12).

---

## 12. Steal / force

This is the theory that explains *why* the design looks the way it does. Two policy
questions define every buffer manager:

- **Steal?** Can a dirty page be written to disk *before* its change is committed (for
  example, evicted to free memory)?
- **Force?** Must every dirty page be written to disk *at* commit?

| | Force | **No-force** |
|---|---|---|
| **Steal** | needs undo | needs undo + redo (ARIES, most real DBs) |
| **No-steal** | needs nothing, but slow | **needs redo only (PersistKV)** |

- **No-steal:** dirty pages are never evicted (`evictIfNeeded` skips them), so the data
  file never contains a half-applied operation. That means **no undo log is needed**.
- **No-force:** pages are not written at commit, only the small WAL record is. Commits
  stay fast, and the WAL provides **redo**.

The trade-off: with no-steal, the set of dirty pages must fit in RAM until the next
checkpoint. Checkpoints that trigger by dirty-page count enforce that.

---

## 13. Testing

### Model-based testing ([tests/unit_tests.cpp](tests/unit_tests.cpp))
Run the same random operations against PersistKV **and** `std::map`, which is known to
be correct, then compare. `random_ops_match_std_map` does 30,000 random puts and deletes
with a tiny 32-page cache, which forces evictions and disk reads, and checks
`checkIntegrity()` along the way.

### Fault injection ([tests/crash_test.cpp](tests/crash_test.cpp), [src/failpoint.cpp](src/failpoint.cpp))
Named **failpoints** are compiled into the engine:
```cpp
failpoint::crashIfHit("ckpt.after_dwb");   // in pager.cpp
```
Normally they do nothing. The test arms one ("crash on the 3rd time you reach
`ckpt.after_dwb`") and runs the engine **as a child process**. When the failpoint fires,
the child calls `_Exit()`, which ends the process instantly with no destructors and no
cleanup, like `kill -9`.

The parent then:
1. reads the child's **ack log** (which operations returned successfully),
2. replays those operations on a `std::map` (the expected state),
3. opens the database, which runs recovery, and
4. requires `checkIntegrity()` to pass and the contents to equal the expected state
   (plus, optionally, the one in-flight operation).

Some failpoints also **simulate torn writes**: they write half a record or half a page
before crashing.

**An honest caveat:** killing a process does not discard the OS page cache, so this
test proves the *logic* (ordering, torn-write handling, replay) rather than physical
power loss. Testing real power loss needs tools like
[dm-log-writes](https://www.kernel.org/doc/html/latest/admin-guide/device-mapper/log-writes.html)
or actually pulling the plug on a VM.

---

## 14. Complexity and capacity

| Operation | Time | Page reads |
|---|---|---|
| get | O(log n) | height (3–4) |
| put | O(log n) amortized | height, plus O(1) amortized splits |
| remove | O(log n) amortized | height, plus siblings on rebalance |
| scan of k keys | O(log n + k) | height + k/entries-per-leaf |
| recovery | O(WAL size + dwb size) | — |

Capacity limits: page ids are 32-bit, which allows 2³² pages × 4 KB = **16 TiB**. Keys
are ≤ 32 bytes and values ≤ 200 bytes.

---

## 15. Limitations

Know these before an interviewer points them out:

| Limitation | Why | What a real system does |
|---|---|---|
| Single-threaded | no locks; old MinGW has no `std::mutex` | Reader-writer locks, or **latch crabbing** (lock a child, then release the parent) |
| Keys ≤ 32 B, values ≤ 200 B | worst-case entries must fit 17 per page | **Slotted pages** with variable-size cells, plus **overflow pages** for big values |
| One fsync per write | simplest correct design | **Group commit**: batch concurrent writers into one fsync |
| No transactions | each put is atomic by itself | Multi-op transactions with undo or MVCC |
| Dirty pages must fit in RAM | no-steal policy | Steal policy + undo logging (ARIES) |
| Checkpoints block writes | single thread | Fuzzy checkpoints in the background |
| No directory fsync on Linux | a brand-new file's entry could be lost on power loss | fsync the directory after creating files |
| Little-endian only | `memcpy` encoding | explicit byte-order encoding |

---

## 16. Interview questions

**Q: Why a B+Tree instead of a hash table?**
Hash tables can't do range queries or ordered iteration. A B+Tree gives O(log n) point
lookups *and* efficient ranges through the linked leaves, and its high fanout keeps the
tree 3–4 levels deep, so a lookup costs few disk reads.

**Q: B-tree vs B+Tree?**
A B+Tree keeps values only in leaves and links the leaves. Internal nodes hold only
keys, so fanout is higher and the tree is shorter. Range scans walk the leaf list
instead of the whole tree.

**Q: What happens on a leaf split vs an internal split?**
A leaf split copies the first key of the new right leaf up into the parent. An internal
split pushes its middle key up and keeps it in neither half. A root split creates a new
root, so the tree grows from the top and stays balanced.

**Q: How do deletes keep the tree balanced?**
If a node drops below half full, it borrows one entry from a sibling with spare
entries, rotating through the parent for internal nodes. Otherwise it merges with a
sibling and removes the separator from the parent, which can cascade upward. If the
root is left with 0 keys, its single child becomes the new root.

**Q: What does "durable" mean, and how do you guarantee it?**
Once `put` returns, the write survives a crash. We append the operation to the WAL and
fsync it *before* returning, and on restart we replay the WAL.

**Q: Why not just fsync the data pages on every write?**
That means several random 4 KB writes per operation, and splits touch even more pages.
A WAL append is one small sequential write. Data pages are flushed later in batches at
checkpoints.

**Q: What is a torn write, and how do you handle it?**
A write that is only partly persisted when power fails. In the WAL, the record CRC fails
and we truncate the log there; the record was never acknowledged. In the data file, the
doublewrite buffer holds a complete copy of every page in the checkpoint, so recovery
rewrites them.

**Q: Why do you need the doublewrite buffer if you have a WAL?**
Our WAL is *logical* (PUT key=value). Replaying it requires a structurally valid tree,
and a crash mid-checkpoint can leave torn or mismatched pages. The doublewrite buffer
guarantees that recovery always starts from a consistent tree. (PostgreSQL solves the
same problem by logging full page images in its WAL instead.)

**Q: Is WAL replay idempotent? What if you crash during recovery?**
Yes. Recovery never modifies the data file until its own checkpoint, and that
checkpoint uses the same doublewrite protocol. Records with LSN ≤ `checkpoint_lsn` are
skipped. Crashing during recovery just means recovery runs again.

**Q: What is an LSN for?**
It orders log records and tells recovery which records are already reflected in the
data file (`lsn <= checkpoint_lsn`), so they are skipped.

**Q: Why don't you need an undo log?**
The buffer pool never writes a dirty page outside a checkpoint (no-steal), and every
operation is fully applied in memory before a checkpoint can run. The data file only
ever holds complete operations, so there is nothing to undo.

**Q: What does the LRU cache evict? Why not dirty pages?**
Only clean pages. Evicting a dirty page would mean writing it to the data file outside
a checkpoint, which breaks the guarantee that the data file is always a consistent
snapshot.

**Q: How did you test crash safety?**
With fault injection. Named crash points in the code kill a child process at exact
moments, including mid-record and mid-page, which simulates torn writes. The parent
recovers the DB and checks it against a model built from the acknowledged operations,
and runs a full invariant check. 14 scenarios pass.

**Q: What does the CRC not protect against?**
Malicious modification, since it is not cryptographic. A corrupted page whose CRC still
happens to match is possible, but the chance is about 1 in 4 billion.

**Q: How would you make it concurrent?**
Simplest: a reader-writer lock on the whole DB. Better: per-page latches with **latch
crabbing**. Take the child's latch, and release the parent's once the child is "safe"
(won't split or merge). Add group commit so concurrent writers share fsyncs.

**Q: How would you support large values?**
Slotted pages (a slot directory at the start of the page, cells packed from the end)
for variable-length entries, plus overflow page chains for values that don't fit.

**Q: B+Tree vs LSM tree?**
An LSM tree buffers writes in memory, then writes sorted runs sequentially and merges
them in the background (compaction). That makes writes very fast, but a read may check
several files. A B+Tree updates in place: reads are faster and more predictable,
writes cost more. RocksDB is an LSM tree; InnoDB is a B+Tree.

**Q: What's the biggest bottleneck right now?**
fsync per write (about 1K durable writes/s), and checkpoints that write every page
twice. Group commit, plus a larger checkpoint interval or a background checkpointer,
would help most.

---

## 17. Code reading order

1. [include/persistkv/common.h](include/persistkv/common.h): constants, error types, byte encoding
2. [src/file.cpp](src/file.cpp): positional read/write and fsync
3. [src/crc32.cpp](src/crc32.cpp): checksums
4. [src/wal.cpp](src/wal.cpp): log append and replay
5. [src/pager.cpp](src/pager.cpp): page cache, free list, checkpoint, doublewrite recovery
6. [src/btree.cpp](src/btree.cpp): the B+Tree (the biggest file; read get → put → remove → check)
7. [src/db.cpp](src/db.cpp): ties it together; the constructor is the recovery algorithm
8. [tests/crash_test.cpp](tests/crash_test.cpp) and [src/failpoint.cpp](src/failpoint.cpp): how crash safety is proven

---

## 18. Further reading

- **CMU 15-445 Database Systems** (free lectures on YouTube). Buffer pools, B+Trees,
  logging and recovery. The closest match to this project.
- **Alex Petrov, *Database Internals*** (O'Reilly). Part I covers B-trees, pages and
  recovery in exactly these terms.
- **Mohan et al., "ARIES"** (1992). The standard recovery algorithm.
- **SQLite: "Atomic Commit In SQLite"** (sqlite.org/atomiccommit.html). An excellent
  explanation of torn writes and journals.
- **Dan Luu, "Files are hard"** (danluu.com/file-consistency). Why fsync correctness is
  difficult in practice.
- **MySQL docs: "InnoDB Doublewrite Buffer"**, and **PostgreSQL docs: `full_page_writes`**.
