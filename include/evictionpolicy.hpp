#pragma once

#include <cstddef>
#include <vector>

struct BufferFrame;

// Chooses which buffer frame to evict when the pool is full. BufferPool
// keeps owning eviction MECHANICS (write-back, resetting the frame) and
// delegates only victim SELECTION here, plus enough bookkeeping hooks
// (recordAccess/reset) to keep each policy's own per-frame state in sync
// with what BufferPool is actually doing -- so BufferFrame itself stays
// policy-agnostic (no ref_bit or similar baked into it).
class EvictionPolicy {
public:
    virtual ~EvictionPolicy() = default;

    // Called on every access to frame `idx` -- a cache hit via
    // fetchPage(), or right after a fresh page is loaded into it. Lets
    // the policy update whatever recency/frequency state it tracks.
    virtual void recordAccess(std::size_t idx) = 0;

    // Called right after frame `idx` has been evicted (or is about to be
    // reused for a brand-new allocation) -- clears any per-frame state
    // the policy was holding for it, so a later recordAccess() on the
    // same slot starts fresh rather than inheriting the previous
    // occupant's history.
    virtual void reset(std::size_t idx) = 0;

    // Returns the index of the frame to evict, or -1 if every frame is
    // currently pinned. BufferPool turns -1 into a thrown exception --
    // this interface has no business knowing that policy.
    virtual int selectVictim(const std::vector<BufferFrame>& frames) = 0;
};

// Classic second-chance clock sweep: a hand walks the frames circularly;
// a frame with its "recently used" bit set gets one pass spared (bit
// cleared, hand advances) before being evicted on a later sweep. Ported
// verbatim from BufferPool::evictFrame()'s pre-Phase-7 inline logic --
// see bufferpool.cpp's history for the original.
class ClockSweepPolicy : public EvictionPolicy {
public:
    void recordAccess(std::size_t idx) override;
    void reset(std::size_t idx) override;
    int selectVictim(const std::vector<BufferFrame>& frames) override;

private:
    std::vector<bool> ref_bits;
    std::size_t clock_hand = 0;

    void ensureSized(std::size_t n);
};
