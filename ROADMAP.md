# DB Engine Roadmap

## Guiding principle
Each phase builds on the previous. Every phase is independently demo-able in an interview.
No shortcuts — each concept mirrors how a real database (PostgreSQL/InnoDB) works under the hood.

---

## ✅ Phase 1: Persist the B+ Tree (COMPLETE)
**What:** Wire up `IndexManager` so the B+ tree survives process restarts.
**Why:** The tree was in-memory — lose power, lose the index. This was the #1 gap.

**Done:**
- `BPlusTree(IndexManager&)` constructor auto-loads from disk if data exists
- `save()` / `load()` persist and reconstruct the full tree (including `next_leaf` chain)
- Every `insert`/`update`/`remove` triggers an automatic full-tree save
- `IndexManager` header stores `root_node_id` so the root is always known after restarts
- Empty tree handled: `root_node_id = -1` on disk
- 10 tests cover: 20-key round-trip, range scan, getAll, update, remove, empty tree

**Systems concept taught:** Serialization, disk layout, the memory-storage boundary.

---

## ✅ Phase 2: Buffer Pool (COMPLETE)
**What:** Replace the current `PageManager` (read/write every call) with a fixed-size page cache using clock-sweep.
**Why:** The #1 performance problem. Every `readPage`/`writePage` hits disk.

**Done:**
- `BufferPool` class owns 64 frames × 4 KB = 256 KB cache
- Each frame: `Page` + `page_id` + `dirty_flag` + `pin_count` + `ref_bit`
- `fetchPage(page_id)` — return from cache or read from disk; pin it
- `unpinPage(page_id, dirty)` — release pin; if dirty, mark for writeback
- Eviction policy: Clock sweep with ref_bit (second-chance algorithm)
- `flush()` — write all dirty pages back to disk
- `PageManager` refactored to use `BufferPool` underneath (transparent to callers)
- 5 tests cover: fetch/unpin cycle, write+readback, eviction pressure (100 pages in 64 frames), dirty flush+reopen, sequential IDs

**Systems concept taught:** Locality of reference, caching, the pin/unpin contract, eviction policy trade-offs.

---

## ✅ Phase 3: B-Tree Concurrency — Latch Crabbing + B-Link (COMPLETE)
**What:** Allow concurrent reads and writes on the B+ tree without corruption.
**Why:** The current tree is single-threaded. Real databases handle thousands of concurrent index operations.

**Done:**
- `RWSpinLatch`: custom atomic-based reader-writer spinlock (not `std::mutex`/`shared_mutex`) with shared/exclusive modes and a non-blocking `tryLock()`, one per `BPlusTreeNode`
- Latch crabbing on every traversal (search/insert/remove/rangeScan): hand-over-hand, child latched before parent released
- B-link design: every node carries a `high_key` and a right-sibling link; `moveRight()` lets a traversal step sideways past an in-progress split instead of blocking on it
- A coarser `structure_latch` (also an `RWSpinLatch`) layered on top: shared for all traversals, exclusive for the full duration of any structural change (split fix-up, remove's borrow/merge) — this is what closes the AB-BA deadlock between a cascading merge's ancestor walk and a concurrent plain insert's descent
- Deadlock avoidance for "backwards" (right-to-left) latch acquisitions (e.g. grabbing a *left* sibling during rebalancing) via conditional `tryLock` + bounded retry rather than a blocking acquire
- Invariant enforced end-to-end: a node's `high_key` always mirrors the separator key one level up (`parent->keys[i] == parent->children[i]->high_key`) — every place a separator is rewritten (borrow, merge, `propagateSeparatorUpdate`) now updates the corresponding child's `high_key` in the same step; a missing update here was the root cause of a real, deterministically-reproducible bug where removes silently no-op'd on certain keys
- Concurrency test suite: disjoint inserts across many threads, mixed insert+search, rangeScan during concurrent inserts, and concurrent insert+remove on disjoint key ranges — all validated for many repeated runs (not just once), since the bugs here were rare enough to hide across dozens of clean runs

**Systems concept taught:** Latches vs. locks, deadlock-free lacing, optimistic vs. pessimistic concurrency, B-link invariants.

---

## ✅ Phase 4: Write-Ahead Log (COMPLETE)
**What:** Before modifying any page, append a log record. On crash recovery, replay committed changes and undo uncommitted ones.
**Why:** Durability. Without it, a crash mid-write corrupts the database.

**Done:**
- Found and fixed a prerequisite gap before any of this could work: `IndexManager` had no incremental writes at all -- `BPlusTree::save()` unconditionally re-serialized and rewrote *every* node in the tree on every single `insert`/`update`/`remove`. A page-granular WAL can't log a meaningful diff for a write that touches everything, so this had to become real incremental persistence first (see below) -- otherwise the WAL would only have protected the heap, not the index.
- `Page` and `BPlusTreeNode` headers gained an 8-byte `lsn` field (the ARIES "page LSN" -- redo only reapplies a record newer than what's already on disk)
- `WALRecord`/`WALWriter` (`include/wal.hpp`, `src/wal.cpp`): length-prefixed, CRC32-checked log records so a torn write at the tail (what a real crash leaves behind) is detected and cleanly discarded rather than misparsed; thread-safe (`WALWriter` and `TransactionManager` are shared between `BufferPool` and `IndexManager`, which can run on different threads)
- `TransactionManager`: minimal auto-commit transaction concept (no multi-statement transactions yet -- that's Phase 5) purely so WAL records have a `txn_id`/`prev_lsn` chain for recovery's undo pass; `txn_id`s stay unique for the WAL file's whole lifetime (scanned from existing log on construction), not just one process's, since recovery has no checkpointing and re-scans the full log every time
- `BufferPool` now captures a before-image at `fetchPage()` time and logs a real before/after-image `UPDATE` record whenever `unpinPage(id, true)` detects the page actually changed, auto-committing internally -- no signature changes needed on `PageManager`/`RecordManager`
- `BPlusTree`'s persistence rewritten from "save the whole tree" to incremental dirty-node writes: a `thread_local` dirty-node set (populated via one-line `markDirty()` calls at each mutation site) replaces threading a parameter through the latch-crabbing call graph, so none of Phase 3's concurrency-critical function signatures or lock ordering changed. `IndexManager::saveNode()` logs the same before/after-image protocol as `BufferPool`, sharing one `WALWriter`/`TransactionManager` (one LSN space, one txn_id space) with the heap path
- `RecoveryManager` (`include/recoverymanager.hpp`, `src/recoverymanager.cpp`): redo pass replays every `UPDATE`/`CLR` record newer than the target page's on-disk LSN (idempotent); undo pass reverts any txn with no `COMMIT` record, in reverse order via `prev_lsn`, writing a CLR per undone step so a crash during recovery is itself redoable
- No checkpointing (full log scan every run) and whole-page (not byte-range) before/after images -- both deliberate v1 simplifications noted as possible Phase 7 optimizations, not correctness gaps
- 16 new tests: `WALWriter` round-trip/corrupt-tail-discard/reopen-continuity, `TransactionManager` prev_lsn chaining, incremental-write verification (a plain insert into a multi-level tree logs ~1 record, not one per node), and crash-recovery tests covering heap redo, heap undo (including data that reached disk under steal despite never committing), index redo/undo, one shared WAL recovering interleaved heap+index writes, and a real end-to-end test using a deliberately-never-destructed `BufferPool` to simulate a crash before write-back
- Full suite (116/116) validated stable across repeated runs, same discipline as Phase 3's concurrency suite

**Systems concept taught:** ARIES fundamentals, REDO/UNDO, crash recovery, the write-ahead invariant, LSN-based page tracking.

---

## ✅ Phase 5: MVCC Transactions (COMPLETE)
**What:** Multi-version concurrency control with undo logs — readers never block writers.
**Why:** Basic 2PL is obsolete. PostgreSQL, InnoDB, and Oracle all use MVCC. It's the industry standard.

**Done:**
- Found and fixed prerequisite gaps before any of this could work: `Table` built a disconnected, in-memory-only `BPlusTree`, silently bypassing all of Phases 1-4's persistence/WAL work; `txn_id` was generated internally by `BufferPool`/`BPlusTree::saveDirty()` and never exposed, so one logical "insert a row and update its index entry" was two independent auto-commit transactions with no shared identity
- `Record` (`include/record.hpp`) carries a fixed MVCC header (`create_txn_id`, `delete_txn_id`, `prev_version` RID) ahead of its field data; `delete_txn_id` sits at a fixed offset so `Page::patchBytes()` can stamp it on an existing on-disk version in place
- `caller_txn_id == 0` is a reserved sentinel meaning "no caller-owned transaction, behave exactly as before Phase 5," threaded as a default parameter through the whole write stack (`BufferPool` → `PageManager` → `RecordManager` → `BPlusTree` → `Table`) — every pre-Phase-5 call site needed zero changes
- Rollback needs no separate physically-replayed undo log: an `UPDATE` never overwrites a version in place, it tombstones the old version's `delete_txn_id` and inserts a new one chained back via `prev_version` — that backward chain of full-tuple before-images *is* the undo log, and it falls out of the write path for free. `RecordManager::updateRecord`/`markDeleted` go through the same fetch/mutate/`writePage` path as everything else, so they're WAL-logged and crash-safe via `BufferPool`'s existing before/after-image diffing with no new recovery machinery
- `MVCCManager` (`include/mvcc.hpp`/`transaction.hpp`) tracks in-memory transaction status and hands out `Snapshot{xmax, active_at_start}`s, and implements `isVisible()` — a from-scratch port of Postgres's tuple visibility rule: read-your-own-writes, an aborted transaction's writes invisible forever, a committed-but-concurrent-with-my-snapshot write invisible until a later snapshot. Deliberately not durable: `RecoveryManager` already guarantees only committed data is on disk by the time anything reads this, so it starts empty every process
- `Table::getByKey` walks the version chain via `isVisible` when the head isn't visible to the caller's snapshot; `insert`/`updateByKey`/`deleteByKey` resolve `txn_id == 0` to a real, immediately-resolved transaction (aborting on exception or "nothing to do" instead of leaving one dangling), so a single statement's heap + index writes are always one atomic, MVCC-stamped unit
- `LockManager` (`include/lockmanager.hpp`): row-level (RID-keyed) exclusive write locks with real OS-level blocking (`std::mutex` + `condition_variable`, not the B-tree's `RWSpinLatch` — a busy-spin design purpose-built for microsecond latch crabbing, wrong for a lock that can be held across a whole multi-statement transaction). MVCC readers never take these locks — only concurrent writers to the same row serialize
- Deadlock detection is a real waits-for graph, not timeout-only: every transaction has at most one outstanding wait, so the graph is a functional graph and cycle detection is a plain chain walk; whichever side detects a cycle first victimizes the numerically younger transaction, so the outcome doesn't depend on scheduling. Timeout remains as a fallback bound for a wait that never resolves any other way
- 21 new tests: read-your-own-writes, snapshot-before-commit-never-sees-it/after-does, aborted-writes-invisible-forever, aborted-delete-un-hides, update chaining with old-snapshot visibility, delete+reinsert index repointing without duplicating entries, crash-recovery tests for both single- and mixed-operation-type transactions, lock manager unit tests (uncontended/re-entrant/disjoint-RID/blocks-then-wakes/timeout), a repeated deadlock test, and a multi-threaded mixed insert/update/delete/read stress test against a shared `Table` proving a repeatable-read snapshot sees no phantoms while writers run concurrently around it
- Full suite (124 → 145) validated stable across repeated runs, same discipline as Phase 3/4

**Systems concept taught:** Snapshot isolation, visibility rules, undo-via-version-chains, row-level locking vs. latching, waits-for-graph deadlock detection, true ACID compliance.

---

## Phase 6: SQL Frontend (≈ 4-5 sessions)
**What:** Accept SQL over TCP and execute it.
**Why:** This is the "oh you built a database" moment.

**Plan:**
- Simple recursive-descent parser: `SELECT/INSERT/CREATE TABLE/DELETE` with WHERE clause (single condition)
- AST → query plan
- Execution: bind to `Table` API, filter rows, project columns
- Wire protocol: simple `\n`-delimited text over TCP (no need for PostgreSQL wire protocol)
- Single-threaded at first, then add connection pool

**Systems concept taught:** Parsing, query planning, iterator model, client-server architecture.

---

## Phase 7: Benchmarking & Polish (ongoing)
**What:** Measure and optimize.
**Why:** Numbers on a resume.

**Plan:**
- Insert throughput (rows/s), point lookup latency, range scan speed
- YCSB-style workload A (50% read / 50% update)
- Compare: no buffer pool vs. buffer pool → shows your cache works
- Compare: single-threaded vs. B-link concurrent → shows your latching works
- **Eviction policy shootout: clock-sweep vs. LRU-2** — implement a pluggable `EvictionPolicy` interface so both policies can be swapped at runtime. LRU-2 tracks the 2nd-most-recent access time per page to prevent scan pollution (a single range scan won't evict hot internal nodes).
- fuzz testing: random operations, assert no crash

---

## Interview Story Arc

| Phase | You can say |
|-------|-------------|
| 1 | "I serialized a B+ tree to disk so indexes survive crashes" |
| 2 | "I built a clock-sweep buffer pool that caches pages and reduces disk I/O by ~100x" |
| 3 | "I added concurrent B-tree access with latch crabbing and B-link, so the tree is safe under 4+ threads" |
| 4 | "I implemented a write-ahead log with ARIES-style crash recovery" |
| 5 | "I built MVCC with undo logs — readers never block writers, exactly like PostgreSQL/InnoDB" |
| 6 | "I wrote a SQL parser and a TCP server so clients can query the database" |

Each phase builds a sentence you can say in an interview. No fluff.

---

## Progress

| Phase | Status |
|-------|--------|
| 1 — Persist the B+ Tree | ✅ Done |
| 2 — Buffer Pool | ✅ Done |
| 3 — B-Tree Concurrency (Latch Crabbing + B-link) | ✅ Done |
| 4 — Write-Ahead Log | ✅ Done |
| 5 — MVCC Transactions | ✅ Done |
| 6 — SQL Frontend | 🔜 Next |
| 7 — Benchmarking & Polish | ⏳ |
