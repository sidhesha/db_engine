#pragma once

#include "db_engine/node.hpp"
#include "db_engine/indexmanager.hpp"
#include "db_engine/latch.hpp"
#include "db_engine/latchhandle.hpp"
#include <memory>
#include <mutex>


// Concurrency design (Phase 3): fine-grained latch crabbing + a B-link
// tree, per ROADMAP.md, not a single global lock.
//
// - Every BPlusTreeNode owns its own RWSpinLatch (node.hpp). Traversals
//   hold at most two latches at a time (current + about to acquire),
//   hand-over-hand ("crabbing"): latch the child, then release the
//   parent.
// - Every node also carries a B-link high_key + right sibling pointer
//   (rightLink()). When a node splits, its new right sibling is linked
//   in immediately, but the parent is fixed up separately/lazily
//   (classic Lehman & Yao). A reader or writer that lands on a node
//   whose high_key the target key exceeds simply moves right instead of
//   missing the key or blocking on the split.
// - insert() therefore has two phases: descend + split the leaf
//   (Phase A), then walk back up fixing parents as needed, using the
//   descent path as a *hint* and re-verifying via the same move-right
//   check at each level in case an ancestor itself split in the
//   meantime (Phase B).
// - Structural modification -- fixing up ancestors after a split
//   (propagateSplit) and rebalancing after a remove causes underflow
//   (borrow/merge across siblings, cascading up) -- is the one place
//   fine-grained latch crabbing alone isn't enough. Confirmed the hard
//   way, with a real trace: a delete-triggered merge reparents a
//   grandchild (setParent) while a concurrent plain insert()'s ordinary
//   hand-over-hand descent can be passing through that same node --
//   because the merge holds its whole root-to-leaf path for the
//   duration of a (possibly multi-level) cascade, this can form a
//   genuine wait-cycle, not just contention. Getting concurrent splits
//   *and* concurrent deletion both fully fine-grained at once is a
//   genuinely hard problem (the original Lehman & Yao paper explicitly
//   scopes deletion out for exactly this reason).
//
//   So structural modification (propagateSplit, remove()) takes
//   structure_latch (a RWSpinLatch, not std::mutex) in EXCLUSIVE mode
//   for its whole duration, and every traversal -- search(),
//   rangeScan(), getAllKeyRIDPairs(), printTree(), and insert()'s
//   descent -- takes it in SHARED mode for its whole duration. This
//   closes the cycle by construction: no traversal can be mid-descent
//   while a structural change is in flight, so a merge can never reach
//   into a node a concurrent descent already holds, and vice versa.
//   Reads and simple inserts still run fully concurrently with each
//   other (shared/shared); only the comparatively rare structural-
//   change path excludes everyone else, which is the standard
//   granularity real lock managers use when concurrent splits and
//   concurrent deletion have to coexist safely.
// - remove()'s rebalancing additionally holds X latches on the whole
//   root-to-leaf path (plus any sibling it touches) for the duration
//   of the call, rather than releasing early.
// - `root` itself is swapped (on root split, or the tree becoming
//   empty/non-empty) under a small dedicated root_guard latch, since no
//   single node's latch protects the root pointer itself.
class BPlusTree {
public:
    BPlusTree();
    explicit BPlusTree(IndexManager& im);

    // txn_id == 0 (default) on insert/update/remove: auto-commit, same
    // as before Phase 5. A real, caller-supplied txn_id groups this
    // tree operation's WAL-logged node writes under a caller-owned,
    // multi-statement transaction (see saveDirty).
    void insert(const Key& key, int page_id, int slot_id, uint64_t txn_id = 0);
    void printTree() const;
    std::optional<RID> search(const Key& key);
    bool update(const Key& key, int new_page_id, int new_slot_id, uint64_t txn_id = 0);
    std::vector<std::pair<Key, RID>> rangeScan(const Key& low, const Key& high);
    std::vector<std::pair<Key, RID>> getAllKeyRIDPairs() const;
    bool remove(const Key& key, uint64_t txn_id = 0);

    void load();

private:
    std::shared_ptr<BPlusTreeNode> root;
    IndexManager* im;

    mutable RWSpinLatch root_guard;      // protects only the `root` pointer itself
    mutable std::mutex io_mutex;         // serializes disk I/O (save()); not a hot-path lock
    // Shared during any traversal (search/rangeScan/getAllKeyRIDPairs/
    // printTree/insert), exclusive during structural modification
    // (propagateSplit, remove()). See class-level comment above.
    mutable RWSpinLatch structure_latch;

    std::shared_ptr<BPlusTreeNode> snapshotRoot() const;
    void swapRoot(std::shared_ptr<BPlusTreeNode> new_root);

    // Invariant: every read or write of a node's `parent` weak_ptr holds
    // that node's own latch (briefly). Without this, a concurrent
    // split's parent-pointer fix-up and a concurrent remove()'s ancestor
    // walk can race on the same weak_ptr with no synchronization at all.
    // Centralized here so node.cpp itself stays latch-agnostic.
    void setParent(const std::shared_ptr<BPlusTreeNode>& child,
                   const std::shared_ptr<BPlusTreeNode>& new_parent);
    std::shared_ptr<BPlusTreeNode> getParent(const std::shared_ptr<BPlusTreeNode>& child) const;

    // Core B-link primitive: while latched `node` has a high_key the
    // target key doesn't satisfy, move right (latch right sibling, then
    // release `node`) and keep checking. Returns the (possibly
    // different) node that should be used next, still latched in the
    // requested mode.
    LatchHandle moveRight(LatchHandle node, const Key& key, bool exclusive) const;

    // Conditionally (non-blocking, with bounded retry) acquires a
    // *left* sibling's latch during remove()'s rebalancing. Left-sibling
    // acquisition goes against the left-to-right ordering every other
    // traversal (moveRight, descent) follows, so blocking here can
    // deadlock against a concurrent reader/writer approaching from the
    // left. Returns an empty (falsy) handle if it couldn't be acquired
    // promptly -- the caller falls back to another rebalancing option
    // rather than risking a cycle.
    LatchHandle tryAcquireSiblingExclusive(std::shared_ptr<BPlusTreeNode> sibling) const;

    // Fallback for propagateSplit when neither ancestor_hints nor
    // getParent(left_child) find a usable parent, and root is already
    // internal (so the root-promotion path doesn't apply either).
    // getParent() walks an inherited-placeholder parent pointer (each
    // split's new sibling briefly borrows its originating node's parent
    // field until fixed up) that can itself be several generations of
    // not-yet-linked nodes deep. This doesn't trust any cached pointer:
    // under the caller's own exclusive structure_latch, root's
    // children[] arrays are the sole authoritative structure, so
    // walking only the rightmost spine (children.back() repeatedly)
    // always reaches left_child's level -- every split, at every depth,
    // extends the current rightmost branch, so any not-yet-linked
    // continuation can only hang off whatever is currently rightmost.
    // Never fans out across sibling branches, so it can't be confused
    // by other branches sitting at a different depth. Returns nullptr
    // if left_child is the current root (caller's is_root check already
    // handles that) or if the tree is structurally inconsistent.
    std::shared_ptr<BPlusTreeNode> findAncestorForRelink(
        const std::shared_ptr<BPlusTreeNode>& left_child) const;

    // Phase B of a split: insert (separator_key -> right_child) into
    // left_child's parent, using `ancestor_hints` (recorded during
    // descent) as a starting point and re-verifying with moveRight in
    // case an ancestor itself split since. Cascades up / creates a new
    // root as needed. Falls back to getParent(left_child) (safe: latched)
    // if the hint doesn't pan out -- provable not to happen for
    // insert-only structural changes, but handled correctly regardless.
    void propagateSplit(std::vector<std::shared_ptr<BPlusTreeNode>>& ancestor_hints,
                        std::shared_ptr<BPlusTreeNode> left_child,
                        std::shared_ptr<BPlusTreeNode> right_child,
                        Key separator_key);

    // Persists exactly the nodes this operation actually touched (see the
    // thread_local dirty-set + markDirty() mechanism in bplustree.cpp),
    // replacing the old "rewrite the whole tree every op" save(). Called
    // once at the end of insert()/update()/remove(), after every latch
    // this call held has been released (saveNode() itself doesn't latch,
    // but WAL logging + IndexManager I/O has no business happening while
    // holding tree latches). All of an operation's dirty nodes share one
    // WAL transaction, so recovery undoes them as a single unit.
    void saveDirty(const std::vector<std::shared_ptr<BPlusTreeNode>>& dirty, uint64_t caller_txn_id = 0);
    std::shared_ptr<BPlusTreeNode> loadRecursive(int node_id);
    void collectLeavesInOrder(std::shared_ptr<BPlusTreeNode> node,
                              std::vector<std::shared_ptr<BPlusTreeNode>>& leaves);
    // high_key/right_link are runtime-only (not persisted -- always
    // derivable from the loaded parent/child/key structure), so a fresh
    // load() must recompute them before the tree is concurrency-safe.
    void rebuildLinks();

    // remove()'s ancestor chain is never rediscovered via `->parent`
    // (which may be mid-update by a concurrent split) -- it's the exact
    // path remove() itself verified while descending, held (X-latched)
    // for the whole call, so it can't go stale underneath it.
    void propagateSeparatorUpdate(std::vector<LatchHandle>& path, const Key& old_sep, const Key& new_sep);
    // Sibling latches used while rebalancing are plain locals, released
    // as soon as each function returns -- never held for the rest of
    // remove()'s call. Holding a merged-away (dead) node's latch longer
    // than necessary was a real, confirmed source of deadlocks during
    // development: it widens the window for an unrelated concurrent
    // operation with a stale reference to collide with it.
    void handleLeafUnderflow(std::vector<LatchHandle>& path);
    void handleInternalUnderflow(std::vector<LatchHandle>& path);
};