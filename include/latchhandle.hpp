#pragma once
#include <memory>
#include <utility>
#include "node.hpp"

// RAII ownership of a latch on a specific node. Exception-safe (always
// releases on destruction unless moved-from or explicitly released) and
// movable, which is what latch-crabbing needs: a handle gets constructed
// on the child before the parent's handle is destroyed/reset, giving
// hand-over-hand lock coupling.
class LatchHandle {
public:
    LatchHandle() = default;

    LatchHandle(std::shared_ptr<BPlusTreeNode> n, bool exclusive)
        : node_(std::move(n)), exclusive_(exclusive) {
        if (node_) {
            if (exclusive_) node_->latch.lock();
            else node_->latch.lockShared();
        }
    }

    // Non-blocking exclusive acquisition: returns an empty (falsy)
    // handle immediately if the latch isn't free, instead of blocking.
    // For acquisitions that go against the usual ordering everything
    // else follows (see RWSpinLatch::tryLock).
    static LatchHandle tryAcquireExclusive(std::shared_ptr<BPlusTreeNode> n) {
        if (!n || !n->latch.tryLock()) return LatchHandle();
        LatchHandle h;
        h.node_ = std::move(n);
        h.exclusive_ = true;
        return h;
    }

    ~LatchHandle() { release(); }

    LatchHandle(LatchHandle&& other) noexcept { *this = std::move(other); }

    LatchHandle& operator=(LatchHandle&& other) noexcept {
        if (this != &other) {
            release();
            node_ = std::move(other.node_);
            exclusive_ = other.exclusive_;
            other.node_ = nullptr;
        }
        return *this;
    }

    LatchHandle(const LatchHandle&) = delete;
    LatchHandle& operator=(const LatchHandle&) = delete;

    void release() {
        if (node_) {
            if (exclusive_) node_->latch.unlock();
            else node_->latch.unlockShared();
            node_ = nullptr;
        }
    }

    BPlusTreeNode* operator->() const { return node_.get(); }
    const std::shared_ptr<BPlusTreeNode>& get() const { return node_; }
    explicit operator bool() const { return static_cast<bool>(node_); }

private:
    std::shared_ptr<BPlusTreeNode> node_;
    bool exclusive_ = false;
};
