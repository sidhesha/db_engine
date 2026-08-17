#include "db_engine/evictionpolicy.hpp"

#include "db_engine/bufferpool.hpp"  // BufferFrame's full definition (pin_count)

void ClockSweepPolicy::ensureSized(std::size_t n) {
    if (ref_bits.size() < n) ref_bits.resize(n, false);
}

void ClockSweepPolicy::recordAccess(std::size_t idx) {
    ensureSized(idx + 1);
    ref_bits[idx] = true;
}

void ClockSweepPolicy::reset(std::size_t idx) {
    ensureSized(idx + 1);
    ref_bits[idx] = false;
}

int ClockSweepPolicy::selectVictim(const std::vector<BufferFrame>& frames) {
    std::size_t n = frames.size();
    ensureSized(n);
    if (clock_hand >= n) clock_hand = 0;

    std::size_t attempts = 0;
    while (attempts < n * 2) {
        std::size_t i = clock_hand;
        clock_hand = (clock_hand + 1) % n;

        if (frames[i].pin_count > 0) {
            attempts++;
            continue;
        }
        if (ref_bits[i]) {
            ref_bits[i] = false;
            attempts++;
            continue;
        }
        return static_cast<int>(i);
    }

    // Fallback: every frame was either pinned or spared by ref_bit within
    // 2n sweeps -- fall back to the first unpinned, occupied frame found
    // by a plain linear scan, ignoring ref_bit. (In practice an empty,
    // never-yet-used frame is always caught by the primary sweep above --
    // it starts with ref_bit false and pin_count 0 -- so by the time this
    // fallback runs, every unpinned frame is already occupied; the
    // `frames[i].page` check is kept anyway to stay exactly faithful to
    // the original inline logic this was ported from.)
    for (std::size_t i = 0; i < n; i++) {
        if (frames[i].pin_count == 0 && frames[i].page) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

void LRU2Policy::ensureSized(std::size_t n) {
    if (history.size() < n) history.resize(n, {0, 0});
}

void LRU2Policy::recordAccess(std::size_t idx) {
    ensureSized(idx + 1);
    std::uint64_t now = ++logical_clock;
    history[idx].second = history[idx].first;
    history[idx].first = now;
}

void LRU2Policy::reset(std::size_t idx) {
    ensureSized(idx + 1);
    history[idx] = {0, 0};
}

int LRU2Policy::selectVictim(const std::vector<BufferFrame>& frames) {
    std::size_t n = frames.size();
    ensureSized(n);

    // Frames accessed fewer than twice are always top eviction priority
    // (standard LRU-K rule) -- among *those*, LRU-2 falls back to plain
    // LRU on their single most-recent access, oldest first. Without this
    // tiebreak, every miss would keep re-selecting the same
    // lowest-index single-access frame (its `second` stays 0 after one
    // more access to a *different* page lands there) instead of
    // spreading across the other untouched/once-touched frames the way
    // an actual cache fill or scan needs to.
    int best_never_twice = -1;
    std::uint64_t oldest_first_time = 0;

    int best_with_history = -1;
    std::uint64_t best_second_time = 0;

    for (std::size_t i = 0; i < n; i++) {
        if (frames[i].pin_count > 0) continue;

        if (history[i].second == 0) {
            if (best_never_twice == -1 || history[i].first < oldest_first_time) {
                best_never_twice = static_cast<int>(i);
                oldest_first_time = history[i].first;
            }
            continue;
        }
        if (best_with_history == -1 || history[i].second < best_second_time) {
            best_with_history = static_cast<int>(i);
            best_second_time = history[i].second;
        }
    }

    if (best_never_twice != -1) return best_never_twice;
    return best_with_history;  // -1 if every frame is pinned
}
