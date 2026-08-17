# Benchmarks

Numbers from `db_engine_bench` (built with `-O2` — see `CMakeLists.txt`; the
`db_engine_test` build stays at `-O0` for fast, debuggable test iteration, so
never quote numbers from that build). Run via:

```powershell
cmake --build build
build\db_engine_bench.exe
```

**Machine:** AMD Ryzen 7 4800HS (8 cores / 16 threads), Windows 11 Home, MinGW
UCRT64 GCC 14.2.0. These are illustrative, single-machine numbers for
interview/resume purposes, not a claim about hardware-independent
performance — re-run locally for your own numbers before quoting them
elsewhere.

## Insert throughput / point lookup latency

Through the full stack (WAL + MVCC + B+ tree index, via `Table`), not an
isolated index number:

| Metric | Result |
|---|---|
| Insert | 5,000 rows in 1.16s → **~4,300 rows/s** |
| Point lookup | 2,000 lookups in 5.2ms → **~2.6 µs/op** avg |

Insert throughput is bounded by the WAL fsync-equivalent flush on every
write-back and the B+ tree's own persistence, which is the honest cost of
this engine's durability guarantees — an insert here is a real, ARIES-style,
crash-safe write, not a bare in-memory hashmap insert.

## Range scan speed

Raw `BPlusTree::rangeScan` (in-memory tree, 5,000 sequential int keys) — no
`Table`-level wrapper exists for range scans, so this measures the
underlying primitive directly:

| Window size | Result |
|---|---|
| 11 keys | 9.2 µs → ~1.2M keys/s |
| 101 keys | 7.4 µs → ~13.6M keys/s |
| 1,001 keys | 44.8 µs → ~22.3M keys/s |

Larger windows amortize the initial descent-to-the-start-key cost, so
throughput climbs with window size — expected B+ tree range-scan behavior.

## YCSB-style workload A (50% read / 50% update)

5,000 rows pre-populated, 5,000 mixed ops at a uniform random key
distribution: **5,000 ops in 1.35s → ~3,700 ops/s**.

## Buffer pool: cache vs. no cache

Same real `BufferPool` code path at `num_frames=1` (evicts on nearly every
access — functionally "no cache") vs. the default 64, 5,000 random fetches
over a 30-page working set (fits entirely in 64 frames, not in 1):

| Configuration | Result |
|---|---|
| 1 frame (no cache) | 433,403 fetches/s |
| 64 frames (cached) | 29,994,001 fetches/s |

**~69x faster cached** — every access after the first cache-fills the
working set entirely in memory; the 1-frame case round-trips through disk
I/O on essentially every fetch.

## Single-threaded vs. concurrent B+ tree insert

20,000 disjoint-key inserts total, split evenly across N threads against
one `BPlusTree` (Phase 3's latch crabbing + B-link design, always on — this
is purely a driver varying its own thread count, not a code path toggle):

| Threads | Result |
|---|---|
| 1 | 786,206 ops/s |
| 2 | 1,044,784 ops/s |
| 4 | 1,004,712 ops/s |
| 8 | 893,683 ops/s |

Honest result, not tuned for effect: there's a real ~33% gain from 1→2
threads, then it flattens and mildly *regresses* at 4 and 8 despite 8 real
cores being available. This is consistent with `bplustree.hpp`'s own
documented tradeoff — `structure_latch` is held exclusively for the full
duration of any structural change (a split's ancestor fix-up, a remove's
rebalancing), which serializes exactly the operations an insert-heavy,
mostly-sequential-key workload triggers most. More threads past 2 just
means more contention on that one latch, not more real parallelism, for
*this specific workload shape*. A read-heavy or wider-keyspace workload
(less split contention) would be expected to scale further — not
benchmarked here, since the roadmap's own ask is the structural
single-vs-concurrent comparison, not a full sensitivity sweep.

## Eviction policy shootout: clock-sweep vs. LRU-2

Workload: a 20-page hot set warmed across 3 rounds (each hot page accessed
≥2 times, giving it a real recent "2nd-most-recent access" time), then a
300-page sequential scan (each page touched exactly once — ~4.7x the
64-frame pool size, guaranteeing real eviction pressure), then the hot set
re-touched once more:

| Policy | Hot pages still resident after the scan |
|---|---|
| Clock-sweep | 0 / 20 (0%) |
| LRU-2 | 20 / 20 (100%) |

This is the exact scenario `ROADMAP.md` names LRU-2 as defending against —
"a single range scan won't evict hot internal nodes." Clock-sweep's
`ref_bit` only buys a page one extra sweep of protection regardless of how
many times it was historically accessed, so a scan long enough to cycle the
clock hand around the pool multiple times evicts every hot page just like
anything else. LRU-2 structurally can't do that: a once-touched scan page
always outranks a twice-touched hot page for eviction, so the entire scan
cycles through scan pages among themselves and never touches the hot set.
Also covered by a real assertion in `test.cpp` (`test_eviction_policy_shootout`),
not just this printed number.
