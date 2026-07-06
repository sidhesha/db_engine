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

## Phase 2: Buffer Pool (≈ 3-4 sessions)
**What:** Replace the current `PageManager` (read/write every call) with a fixed-size page cache using clock-sweep.
**Why:** The #1 performance problem. Every `readPage`/`writePage` hits disk.

**Plan:**
- `BufferPool` class owns N frames (e.g., 64 frames × 4 KB = 256 KB cache)
- Each frame: `Page` + `page_id` + `dirty_flag` + `pin_count` + `last_access`
- `fetchPage(page_id)` — return from cache or read from disk; pin it
- `unpinPage(page_id, dirty)` — release pin; if dirty, mark for eventual write
- Eviction policy: Clock sweep (simpler than LRU, still shows you understand)
- `flush()` — write all dirty pages back to disk
- Refactor `PageManager` to use `BufferPool` underneath (or replace it entirely)

**Systems concept taught:** Locality of reference, caching, the pin/unpin contract, eviction policy trade-offs.

---

## Phase 3: B-Tree Concurrency — Latch Crabbing + B-Link (≈ 3-4 sessions)
**What:** Allow concurrent reads and writes on the B+ tree without corruption.
**Why:** The current tree is single-threaded. Real databases handle thousands of concurrent index operations.

**Plan:**
- Latch crabbing (lock-coupling): acquire latch on parent, then child, release parent — traverse safely
- Replace `std::shared_ptr` children with stable page IDs so structural changes don't invalidate in-progress traversals
- B-link variant: each internal node stores a "high key" and a link to a right sibling, so splits don't block readers
- S/X latches on nodes (not std::mutex — spinlock or futex-based for low overhead)
- Test: 4 threads hammering inserts + lookups simultaneously; no lost keys, no crashes

**Systems concept taught:** Latches vs. locks, deadlock-free lacing, optimistic vs. pessimistic concurrency, B-link invariants.

---

## Phase 4: Write-Ahead Log (≈ 3-4 sessions)
**What:** Before modifying any page, append a log record. On crash recovery, replay committed changes and undo uncommitted ones.
**Why:** Durability. Without it, a crash mid-write corrupts the database.

**Plan:**
- Log format: `LSN | prev_LSN | txn_id | page_id | offset | old_data | new_data`
- `WALWriter` — append-only sequential file with LSN tracking
- Every `BufferPool::unpinPage(dirty=true)` forces a log write first (WAL rule: write-ahead)
- Recovery: read log forward; redo all changes; undo incomplete txns by writing before-images
- Test: write records, corrupt the DB file (simulate crash), restart, assert data is intact

**Systems concept taught:** ARIES fundamentals, REDO/UNDO, crash recovery, the write-ahead invariant, LSN-based page tracking.

---

## Phase 5: MVCC Transactions (≈ 4-5 sessions)
**What:** Multi-version concurrency control with undo logs — readers never block writers.
**Why:** Basic 2PL is obsolete. PostgreSQL, InnoDB, and Oracle all use MVCC. It's the industry standard.

**Plan:**
- `Transaction` class: `txn_id`, `state` (ACTIVE/COMMITTED/ABORTED), `snapshot`
- Undo log: append-only chain of before-images per transaction
- Each page header stores a `last_LSN` to coordinate with WAL
- Row-level visibility: each record carries `create_txn_id` / `delete_txn_id`
- Snapshot isolation: a transaction sees rows committed before its start timestamp
- Lock manager for serializable conflicts (still needed for predicate locking)
- Deadlock detection: waits-for graph with timeout
- On `ROLLBACK`: walk the undo log, restore before-images
- Test: multiple threads reading/writing concurrently; phantom-free snapshots

**Systems concept taught:** Snapshot isolation, visibility rules, undo logging, the read-set/write-set problem, true ACID compliance.

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
| 2 — Buffer Pool | 🔜 Next |
| 3 — B-Tree Concurrency (Latch Crabbing + B-link) | ⏳ |
| 4 — Write-Ahead Log | ⏳ |
| 5 — MVCC Transactions | ⏳ |
| 6 — SQL Frontend | ⏳ |
| 7 — Benchmarking & Polish | ⏳ |
