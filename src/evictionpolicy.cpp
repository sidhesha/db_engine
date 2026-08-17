#include "evictionpolicy.hpp"

#include "bufferpool.hpp"  // BufferFrame's full definition (pin_count)

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
