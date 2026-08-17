// Benchmark harness (Phase 7). Separate executable from db_engine_test:
// a slow or uninteresting number here is not a build failure, and these
// want to run at real optimization levels (see CMakeLists.txt forcing
// -O2 for this target specifically), unlike the -O0 debug build the
// correctness test suite uses.
#include <iostream>
#include <string>
#include <vector>

#include "bufferpool.hpp"
#include "evictionpolicy.hpp"

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

int main() {
    runEvictionShootout();
    return 0;
}
