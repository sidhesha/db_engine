# DB Engine Roadmap

## Guiding principle
Each phase builds on the previous, and every phase is independently demo-able. No shortcuts —
each concept mirrors how a real database (PostgreSQL/InnoDB) works under the hood.

## What each phase proves

| Phase | | |
|---|---|---|
| 1 | Persist the B+ Tree | Serialized a B+ tree to disk so indexes survive crashes |
| 2 | Buffer Pool | Built a clock-sweep buffer pool that caches pages and cuts disk I/O by ~100x |
| 3 | B-Tree Concurrency | Real latch crabbing + B-link design — the tree is safe under 4+ concurrent threads |
| 4 | Write-Ahead Log | ARIES-style crash recovery: redo/undo, torn-write detection, idempotent replay |
| 5 | MVCC Transactions | Snapshot isolation, readers never block writers — the same model Postgres/InnoDB use |
| 6 | SQL Frontend | A real parser and TCP server, so clients can query the database with actual SQL |
| 7 | Benchmarking & Polish | A pluggable eviction policy, real numbers, and a model-based fuzzer to back it all up |

Every phase ships with tests and, where it matters, a benchmark or a found-and-fixed bug —
not just "it compiles." Full detail below; `docs/BENCHMARKS.md` and `docs/CONCURRENCY_BUGS.md`
for the receipts.

## Progress

| Phase | Status |
|-------|--------|
| 1 — Persist the B+ Tree | ✅ Done |
| 2 — Buffer Pool | ✅ Done |
| 3 — B-Tree Concurrency (Latch Crabbing + B-link) | ✅ Done |
| 4 — Write-Ahead Log | ✅ Done |
| 5 — MVCC Transactions | ✅ Done |
| 6 — SQL Frontend | ✅ Done |
| 7 — Benchmarking & Polish | ✅ Done |

---

## ✅ Phase 1: Persist the B+ Tree
**Why:** The tree was in-memory — lose power, lose the index.

- `BPlusTree(IndexManager&)` auto-loads from disk; `save()`/`load()` reconstruct the full tree,
  including the `next_leaf` chain
- Every `insert`/`update`/`remove` persists automatically; empty-tree state (`root_node_id = -1`)
  handled explicitly
- 10 tests: round-trip, range scan, `getAll`, update, remove, empty tree

**Systems concept:** Serialization, disk layout, the memory-storage boundary.

---

## ✅ Phase 2: Buffer Pool
**Why:** Every `readPage`/`writePage` was hitting disk — the #1 performance problem.

- `BufferPool`: 64 frames × 4 KB, clock-sweep (second-chance) eviction, pin/unpin contract
- `PageManager` refactored to sit on top of it transparently — no caller changes
- 5 tests: fetch/unpin cycle, write+readback, eviction under pressure (100 pages, 64 frames),
  dirty flush + reopen, sequential IDs

**Systems concept:** Locality of reference, caching, eviction trade-offs.

---

## ✅ Phase 3: B-Tree Concurrency — Latch Crabbing + B-Link
**Why:** A single-threaded tree can't do what real databases do — thousands of concurrent index ops.

- Custom atomic reader-writer spinlock (`RWSpinLatch`) per node, not `std::mutex` — cheap enough
  to hold many short-lived latches per operation
- Hand-over-hand latch crabbing on every traversal; a B-link design (`high_key` + right-sibling
  pointer) lets a reader step past an in-progress split instead of blocking on it
- A coarser `structure_latch` scopes structural changes (splits, merges) to close a real AB-BA
  deadlock between a cascading merge and a concurrent insert — found via a reproducible trace, not
  guessed at
- Concurrency suite (disjoint inserts, mixed insert+search, concurrent insert+remove) run for many
  repeated iterations, since the bugs here were rare enough to hide across dozens of clean runs

**Systems concept:** Latches vs. locks, deadlock-free lacing, B-link invariants.

---

## ✅ Phase 4: Write-Ahead Log
**Why:** Without durability, a crash mid-write corrupts the database.

- Found the real prerequisite gap first: the index had no incremental persistence at all — every
  op rewrote the *entire* tree, which a page-granular WAL can't log a meaningful diff for. Fixed
  that before anything else could work.
- ARIES-style `WALRecord`/`WALWriter`: length-prefixed, CRC-checked, so a torn write at the tail
  (what a real crash leaves behind) is detected and discarded, not misparsed
- Redo replays anything newer than what's on disk (idempotent); undo reverts uncommitted
  transactions in reverse via `prev_lsn`, itself logged as CLRs so a crash *during* recovery is
  still recoverable
- One shared LSN/txn_id space across the heap and index paths
- 16 new tests, including heap+index crash recovery and a real never-flushed-`BufferPool` crash
  simulation. 116/116 passing, stable across repeated runs.

**Systems concept:** ARIES fundamentals, REDO/UNDO, LSN-based page tracking.

---

## ✅ Phase 5: MVCC Transactions
**Why:** Basic locking is obsolete — Postgres, InnoDB, and Oracle all use MVCC.

- Rollback needs no separate undo log: an update tombstones the old version and chains a new one
  via `prev_version` — that backward chain of before-images *is* the undo log, for free
- `MVCCManager` ports Postgres's tuple visibility rule from scratch: read-your-own-writes, aborted
  writes invisible forever, committed-but-concurrent writes invisible until a later snapshot
  (`Snapshot{xmax, active_at_start}`)
- Row-level `LockManager` with real OS-level blocking, and **deadlock detection via an actual
  waits-for graph** — not a timeout guess. Victim selection is deterministic regardless of thread
  scheduling.
- 21 new tests: visibility rules, crash recovery for multi-statement transactions, a repeated
  deadlock test, and a multi-threaded stress test proving a repeatable-read snapshot sees zero
  phantoms while writers run concurrently. 124 → 145 passing.

**Systems concept:** Snapshot isolation, undo-via-version-chains, waits-for-graph deadlock detection.

---

## ✅ Phase 6: SQL Frontend
**Why:** This is the "oh, you built a database" moment.

- Multi-table storage sharing one WAL, Postgres-style: every record tagged with a `table_id`, so a
  transaction spanning multiple tables commits atomically — chosen deliberately over a simpler
  per-table-WAL design
- Hand-rolled recursive-descent `Lexer`/`Parser`: `CREATE TABLE / INSERT / SELECT / DELETE /
  UPDATE / BEGIN / COMMIT / ROLLBACK`, single-condition `WHERE`, clean errors on malformed input
- `execute()` binds the parsed AST straight to the storage engine and never throws — every failure
  comes back as a typed result
- `SqlServer`: a real multi-threaded TCP server, one thread per connection. Concurrent clients
  genuinely serialize on row locks and see correct snapshot isolation — through SQL, not just at
  the internal API — because the engine underneath actually is concurrency-safe.
- 33 new tests (145 → 178): a write-write conflict between two live SQL connections blocking and
  resolving, repeatable-read over a real socket, multi-table crash recovery through the full SQL
  stack.

**Systems concept:** Parsing, query planning, client-server architecture, multi-table WAL design.

---

## ✅ Phase 7: Benchmarking & Polish
**Why:** Numbers on a resume.

- Pluggable `EvictionPolicy`: clock-sweep extracted with zero behavior change, then LRU-2 added.
  **Proven, not just implemented** — a benchmark warms a hot set, runs a sequential scan ~5x the
  pool size, and re-checks it: clock-sweep drops to 0% retained, LRU-2 holds 100%. Real assertion,
  not a printed number.
- Same real `BufferPool` at 1 frame vs. 64 shows a genuine cache-vs-no-cache comparison — **~69x**
  more fetches/s cached — without maintaining a second no-cache implementation
- Model-based fuzz testing: a shadow model cross-checked against thousands of random operations,
  catching silent wrong-answer bugs, not just crashes
- Full numbers (insert throughput, point lookup latency, range scan, YCSB workload A) in
  `docs/BENCHMARKS.md`, including an honest account of where concurrency *doesn't* scale and why
- Also ported the whole engine to build and run on Linux (Winsock2/POSIX behind one compat shim,
  CI on both platforms) — what makes the
  [live web console](https://sidhesha.github.io/db_engine-console/) possible

**Systems concept:** Benchmarking methodology, LRU-K eviction, model-based fuzz testing.
