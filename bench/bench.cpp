// Benchmark harness (Phase 7). Separate executable from db_engine_test:
// a slow or uninteresting number here is not a build failure, and these
// want to run at real optimization levels (see CMakeLists.txt forcing
// -O2 for this target specifically), unlike the -O0 debug build the
// correctness test suite uses.
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "db_engine/bplustree.hpp"
#include "db_engine/bufferpool.hpp"
#include "db_engine/database.hpp"
#include "db_engine/evictionpolicy.hpp"

namespace {

// Same shape of workload as test.cpp's test_eviction_policy_shootout(),
// but a standalone copy: the test's parameters are tuned to guarantee a
// deterministic pass/fail, while this one just wants to print real
// numbers -- coupling them would make either purpose harder to read.
int runEvictionWorkload(EvictionPolicy& policy, std::size_t num_frames, int hot_set, int scan_len,
                         int warmup_rounds) {
    std::vector<BufferFrame> frames(num_frames);
    std::vector<int> resident(num_frames, -1);

    auto access = [&](int page_id) -> bool {
        for (std::size_t i = 0; i < num_frames; i++) {
            if (resident[i] == page_id) {
                policy.recordAccess(i);
                return true;
            }
        }
        int idx = policy.selectVictim(frames);
        resident[static_cast<std::size_t>(idx)] = page_id;
        policy.reset(static_cast<std::size_t>(idx));
        policy.recordAccess(static_cast<std::size_t>(idx));
        return false;
    };

    for (int round = 0; round < warmup_rounds; round++) {
        for (int i = 0; i < hot_set; i++) access(1000 + i);
    }
    for (int i = 0; i < scan_len; i++) access(2000 + i);

    int hits = 0;
    for (int i = 0; i < hot_set; i++) {
        if (access(1000 + i)) hits++;
    }
    return hits;
}

void printRow(const std::string& label, int hits, int hot_set) {
    double rate = 100.0 * hits / hot_set;
    std::cout << "  " << label << ": " << hits << "/" << hot_set
              << " hot pages still resident after the scan (" << rate << "% hit rate)\n";
}

}  // namespace

void runEvictionShootout() {
    std::cout << "=== Eviction Policy Shootout: clock-sweep vs. LRU-2 ===\n";
    std::cout << "Workload: a 20-page hot set warmed across 3 rounds, then a 300-page\n"
                 "sequential scan (each scan page touched exactly once), then the hot\n"
                 "set re-touched once more. 64 frames total -- the scan alone is\n"
                 "~4.7x the pool size, so real eviction pressure is guaranteed.\n\n";

    constexpr std::size_t NUM_FRAMES = 64;
    constexpr int HOT_SET = 20;
    constexpr int SCAN_LEN = 300;
    constexpr int WARMUP_ROUNDS = 3;

    ClockSweepPolicy clock_policy;
    LRU2Policy lru2_policy;

    int clock_hits = runEvictionWorkload(clock_policy, NUM_FRAMES, HOT_SET, SCAN_LEN, WARMUP_ROUNDS);
    int lru2_hits = runEvictionWorkload(lru2_policy, NUM_FRAMES, HOT_SET, SCAN_LEN, WARMUP_ROUNDS);

    printRow("clock-sweep", clock_hits, HOT_SET);
    printRow("LRU-2      ", lru2_hits, HOT_SET);
    std::cout << "\n";
}

// ─── Insert throughput / point lookup latency (via Table -- real numbers
// through the full WAL+MVCC+B-tree stack, not an isolated index number) ──

void runThroughputAndLatency() {
    std::cout << "=== Insert Throughput / Point Lookup Latency ===\n";

    const std::string dir = "bench_data_throughput";
    std::filesystem::remove_all(dir);
    // Database (and the Table reference into it) scoped to this block so
    // both are fully destructed -- closing every open file handle --
    // before the trailing remove_all() below runs; removing the
    // directory while a PageManager still has a file open fails on
    // Windows.
    {
    Database db(dir);
    db.createTable("bench", Schema(std::vector<Column>{{"id", "int"}, {"value", "string"}}));
    Table& t = db.getTable("bench");

    constexpr int N = 5000;
    std::vector<std::string> keys(N);
    for (int i = 0; i < N; i++) keys[i] = std::to_string(i);

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; i++) {
        t.insert(std::vector<std::string>{keys[i], "value_" + std::to_string(i)});
    }
    auto t1 = std::chrono::steady_clock::now();
    double insert_secs = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "  insert: " << N << " rows in " << insert_secs << "s ("
              << static_cast<long long>(N / insert_secs) << " rows/s)\n";

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, N - 1);
    constexpr int LOOKUPS = 2000;

    auto t2 = std::chrono::steady_clock::now();
    for (int i = 0; i < LOOKUPS; i++) {
        auto rec = t.getByKey(keys[static_cast<std::size_t>(dist(rng))]);
        (void)rec;
    }
    auto t3 = std::chrono::steady_clock::now();
    double lookup_secs = std::chrono::duration<double>(t3 - t2).count();
    std::cout << "  point lookup: " << LOOKUPS << " lookups in " << lookup_secs << "s ("
              << (lookup_secs / LOOKUPS * 1e6) << " us/op avg)\n\n";
    }
    std::filesystem::remove_all(dir);
}

// ─── Range scan speed (raw BPlusTree -- Table has no range-scan wrapper,
// so this measures the same underlying BPlusTree::rangeScan directly) ────

void runRangeScanBenchmark() {
    std::cout << "=== Range Scan Speed ===\n";

    constexpr int N = 5000;
    BPlusTree tree;
    for (int i = 0; i < N; i++) tree.insert(Key(i), i, i);

    for (int window : {10, 100, 1000}) {
        int lo = (N - window) / 2;
        int hi = lo + window;

        auto t0 = std::chrono::steady_clock::now();
        auto results = tree.rangeScan(Key(lo), Key(hi));
        auto t1 = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(t1 - t0).count();

        std::cout << "  " << results.size() << "-key range scan in " << (secs * 1e6) << " us ("
                  << static_cast<long long>(results.size() / secs) << " keys/s)\n";
    }
    std::cout << "\n";
}

// ─── Buffer pool: cache vs. no cache ──────────────────────────────────────
// Same real BufferPool at num_frames=1 (evicts on nearly every access,
// functionally "no cache") vs. the default 64 -- see bufferpool.hpp's
// constructor comment for why this is a real comparison rather than a
// second, parallel no-cache implementation.

void runBufferPoolComparison() {
    std::cout << "=== Buffer Pool: no cache (1 frame) vs. cached (64 frames) ===\n";

    const std::string dir = "bench_data_bufferpool";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    constexpr int WORKING_SET = 30;  // fits entirely in 64 frames, not in 1
    constexpr int ACCESSES = 5000;

    auto runOnce = [&](const std::string& label, int num_frames) {
        std::string file = dir + "/pool_" + std::to_string(num_frames) + ".heap";
        std::string wal_file = dir + "/pool_" + std::to_string(num_frames) + ".wal";

        WALWriter wal(wal_file);
        TransactionManager txns(wal);
        BufferPool bp(file, wal, txns, 0, std::make_unique<ClockSweepPolicy>(), num_frames);

        std::vector<int> page_ids;
        for (int i = 0; i < WORKING_SET; i++) page_ids.push_back(bp.allocatePage());

        std::mt19937 rng(123);
        std::uniform_int_distribution<int> dist(0, WORKING_SET - 1);

        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < ACCESSES; i++) {
            int pid = page_ids[static_cast<std::size_t>(dist(rng))];
            Page& p = bp.fetchPage(pid);
            (void)p;
            bp.unpinPage(pid, false);
        }
        auto t1 = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(t1 - t0).count();

        std::cout << "  " << label << ": " << ACCESSES << " random fetches over a "
                  << WORKING_SET << "-page working set in " << secs << "s ("
                  << static_cast<long long>(ACCESSES / secs) << " fetches/s)\n";
    };

    runOnce("no cache (1 frame) ", 1);
    runOnce("cached (64 frames) ", 64);

    std::cout << "\n";
    std::filesystem::remove_all(dir);
}

// ─── Single-threaded vs. concurrent B-link insert ─────────────────────────
// No code path toggles concurrency on/off -- Phase 3's latch crabbing is
// unconditional on every BPlusTree operation. The comparison is purely
// this driver varying its own thread count against the same tree design.

void runConcurrencyComparison() {
    std::cout << "=== Single-threaded vs. Concurrent B+Tree Insert ===\n";

    constexpr int TOTAL_KEYS = 20000;

    for (int nthreads : {1, 2, 4, 8}) {
        BPlusTree tree;
        int per_thread = TOTAL_KEYS / nthreads;

        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> threads;
        for (int ti = 0; ti < nthreads; ti++) {
            threads.emplace_back([&tree, ti, per_thread]() {
                for (int i = 0; i < per_thread; i++) {
                    int k = ti * per_thread + i;
                    tree.insert(Key(k), k, k);
                }
            });
        }
        for (auto& th : threads) th.join();
        auto t1 = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(t1 - t0).count();

        int total = per_thread * nthreads;
        std::cout << "  " << nthreads << " thread(s): " << total << " inserts in " << secs
                  << "s (" << static_cast<long long>(total / secs) << " ops/s)\n";
    }
    std::cout << "\n";
}

// ─── YCSB-style workload A: 50% read / 50% update ─────────────────────────

void runYcsbWorkloadA() {
    std::cout << "=== YCSB-style Workload A (50% read / 50% update) ===\n";

    const std::string dir = "bench_data_ycsb";
    std::filesystem::remove_all(dir);
    // See runThroughputAndLatency()'s comment: Database must be fully
    // destructed before remove_all() below runs.
    {
    Database db(dir);
    db.createTable("ycsb", Schema(std::vector<Column>{{"id", "int"}, {"value", "string"}}));
    Table& t = db.getTable("ycsb");

    constexpr int N = 5000;
    std::vector<std::string> keys(N);
    for (int i = 0; i < N; i++) {
        keys[i] = std::to_string(i);
        t.insert(std::vector<std::string>{keys[i], "initial"});
    }

    std::mt19937 rng(7);
    std::uniform_int_distribution<int> key_dist(0, N - 1);
    std::uniform_int_distribution<int> op_dist(0, 1);  // 0 = read, 1 = update
    constexpr int OPS = 5000;

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < OPS; i++) {
        const std::string& key = keys[static_cast<std::size_t>(key_dist(rng))];
        if (op_dist(rng) == 0) {
            auto rec = t.getByKey(key);
            (void)rec;
        } else {
            t.updateByKey(key, std::vector<std::string>{key, "updated_" + std::to_string(i)});
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    std::cout << "  " << OPS << " ops over " << N << " rows in " << secs << "s ("
              << static_cast<long long>(OPS / secs) << " ops/s)\n\n";
    }
    std::filesystem::remove_all(dir);
}

int main() {
    runThroughputAndLatency();
    runRangeScanBenchmark();
    runYcsbWorkloadA();
    runBufferPoolComparison();
    runConcurrencyComparison();
    runEvictionShootout();
    return 0;
}
