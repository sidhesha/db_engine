# DB Engine Roadmap

## Guiding principle
Each phase builds on the previous. Every phase is independently demo-able in an interview.

---

## Phase 1: Persist the B+ Tree  (≈ 2-3 coding sessions)
**What:** Wire up `IndexManager` so the B+ tree survives process restarts.
**Why:** Currently the tree is in-memory — lose power, lose the index. This is the #1 gap.

**Plan:**
- `BPlusTree` gets a `save()` / `load(IndexManager&)` or a constructor that takes `IndexManager&` and auto-loads
- On every insert/remove/update, persist the affected nodes (or lazy-write on flush)
- `IndexManager` already has `loadNode`, `saveNode`, `allocateNodeID` — just needs connecting
- Add a test that inserts 20 keys, destroys the tree, reloads from disk, asserts all keys survive

**Systems concept taught:** Serialization, disk layout, the memory-storage boundary.

---

## Phase 2: Buffer Pool (≈ 3-4 sessions)
**What:** Replace the current `PageManager` (read/write every call) with a fixed-size page cache using clock-sweep or LRU.
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

## Phase 3: Write-Ahead Log (≈ 3-4 sessions)
**What:** Before modifying any page, append a log record. On crash recovery, replay committed changes and undo uncommitted ones.
**Why:** Durability. Without it, a crash mid-write corrupts the database.

**Plan:**
- Log format: `LSN | prev_LSN | txn_id | page_id | offset | old_data | new_data` (or simpler: `page_id | before_image | after_image`)
- `WALWriter` — append-only sequential file
- Every `BufferPool::unpinPage(dirty=true)` forces a log write first (WAL rule: write-ahead)
- Recovery: read log forward; redo all changes; undo incomplete txns by writing before-images
- Test: write records, corrupt the DB file (simulate crash), restart, assert data is intact

**Systems concept taught:** ARIES fundamentals, REDO/UNDO, crash recovery, the write-ahead invariant.

---

## Phase 4: Simple Transactions (≈ 2-3 sessions)
**What:** `BEGIN`, `COMMIT`, `ROLLBACK` with row-level locking.
**Why:** Concurrency control — the other half of ACID.

**Plan:**
- `Transaction` class: `txn_id`, `state` (ACTIVE/COMMITTED/ABORTED), `lock_set`
- Lock manager: shared (read) / exclusive (write) locks on RIDs
- Deadlock detection: timeout or waits-for graph
- On `ROLLBACK`: apply UNDO using the WAL
- Test: two threads inserting simultaneously; no lost updates, no dirty reads

**Systems concept taught:** Isolation levels, 2PL, deadlock, serializability.

---

## Phase 5: SQL Frontend (≈ 4-5 sessions)
**What:** Accept SQL over TCP and execute it.
**Why:** This is the "oh you built a database" moment.

**Plan:**
- Simple recursive-descent parser: `SELECT/INSERT/CREATE TABLE/DELETE` with WHERE clause (single condition)
- AST → query plan
- Execution: bind to `Table` API, filter rows, project columns
- Wire protocol: simple `\n`-delimited text over TCP (no need for PostgreSQL wire protocol)
- Single-threaded at first, then add connection pool

**Systems concept taught:** Parsing, query planning, iterator model (next time), client-server architecture.

---

## Phase 6: Benchmarking & Polish (ongoing)
**What:** Measure and optimize.
**Why:** Numbers on a resume.

**Plan:**
- Insert throughput (rows/s), point lookup latency, range scan speed
- YCSB-style workload A (50% read / 50% update)
- Compare: no buffer pool vs. buffer pool → shows your cache works
- fuzz testing: random operations, assert no crash

---

## Interview Story Arc

| Phase | You can say |
|-------|-------------|
| 1 | "I serialized a B+ tree to disk so indexes survive crashes" |
| 2 | "I built an LRU buffer pool to cache pages and reduce disk I/O by 100x" |
| 3 | "I implemented a write-ahead log with crash recovery" |
| 4 | "I added transactions with row-level locking and deadlock detection" |
| 5 | "I wrote a SQL parser and a TCP server so clients can query the database" |

Each phase builds a sentence you can say in an interview. No fluff.

---

## Recommendation

Start with **Phase 1** — it's the smallest change with the biggest impact. The `IndexManager` stub already exists; you just need to hook it into `BPlusTree`. Once that's done, everything else unlocks.
