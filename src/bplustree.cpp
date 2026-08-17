#include "bplustree.hpp"
#include "constants.hpp"
#include "key.hpp"
#include <algorithm>
#include <queue>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

// Which nodes the current thread's in-flight insert()/update()/remove()
// call has actually mutated (created, split, merged into, had a key/rid/
// child/separator change). Set up at the top of each of those calls and
// torn down at the end, right around the point that used to call
// maybeSave(). A thread_local pointer -- rather than threading a
// DirtySet& parameter through propagateSplit/handleLeafUnderflow/
// handleInternalUnderflow/propagateSeparatorUpdate -- so this bookkeeping
// stays entirely additive: no signature changes, no new latch
// acquisitions, nothing that could alter the latch-crabbing control flow
// those functions already got right in Phase 3. Each top-level call runs
// on one thread and never crosses threads, so thread_local scoping is
// exactly right (and concurrent operations on the same tree, each on
// their own thread, get their own independent dirty set).
thread_local std::vector<std::shared_ptr<BPlusTreeNode>>* g_dirty_set = nullptr;

void markDirty(const std::shared_ptr<BPlusTreeNode>& node) {
    if (!g_dirty_set || !node) return;
    for (auto& n : *g_dirty_set) {
        if (n.get() == node.get()) return;  // already marked this operation
    }
    g_dirty_set->push_back(node);
}

// RAII scope for g_dirty_set: installs `dirty` as the active set for the
// current thread's operation, restores the previous value (always
// nullptr in practice, since these calls don't nest) on scope exit --
// including via an exception, so a thrown error mid-operation can't leave
// a stale pointer for the next call on this thread.
struct DirtyScope {
    std::vector<std::shared_ptr<BPlusTreeNode>>* previous;
    explicit DirtyScope(std::vector<std::shared_ptr<BPlusTreeNode>>& dirty) : previous(g_dirty_set) {
        g_dirty_set = &dirty;
    }
    ~DirtyScope() { g_dirty_set = previous; }
};

}  // namespace


BPlusTree::BPlusTree()
    : im(nullptr) {
    root = std::make_shared<BPlusTreeNode>(true);  // root is leaf at start
}

BPlusTree::BPlusTree(IndexManager& indexManager)
    : im(&indexManager) {
    if (im->hasData()) {
        load();
    } else {
        root = std::make_shared<BPlusTreeNode>(true);
    }
}

std::shared_ptr<BPlusTreeNode> BPlusTree::snapshotRoot() const {
    root_guard.lockShared();
    auto r = root;
    root_guard.unlockShared();
    return r;
}

void BPlusTree::swapRoot(std::shared_ptr<BPlusTreeNode> new_root) {
    root_guard.lock();
    root = new_root;
    root_guard.unlock();
}

void BPlusTree::setParent(const std::shared_ptr<BPlusTreeNode>& child,
                          const std::shared_ptr<BPlusTreeNode>& new_parent) {
    if (!child) return;
    LatchHandle g(child, true);
    child->parent = new_parent;
}

std::shared_ptr<BPlusTreeNode> BPlusTree::getParent(const std::shared_ptr<BPlusTreeNode>& child) const {
    if (!child) return nullptr;
    std::shared_ptr<BPlusTreeNode> p;
    {
        LatchHandle g(child, false);
        p = child->parent.lock();
    }
    if (!p) return nullptr;
    // child->parent is a placeholder inherited at split time and only
    // gets fixed up once that node's own propagateSplit call runs --
    // until then it can point at an ancestor a concurrent remove() has
    // since merged into a sibling. handleLeafUnderflow/
    // handleInternalUnderflow never clear a merged-away node's own
    // fields, so a dead parent would otherwise still look perfectly
    // valid (real latch, real children) to a caller that only checks
    // for null. Latched and released separately from child's own latch
    // (never held together) to keep the same child-before-parent
    // ordering every other traversal in this file uses.
    bool dead;
    {
        LatchHandle gp(p, false);
        dead = p->is_merged_away;
    }
    return dead ? nullptr : p;
}

LatchHandle BPlusTree::moveRight(LatchHandle node, const Key& key, bool exclusive) const {
    while (node->high_key.has_value() && key >= *node->high_key) {
        auto right = node->rightLink();
        if (!right) break;
        LatchHandle right_handle(right, exclusive);
        node.release();
        node = std::move(right_handle);
    }
    return node;
}

LatchHandle BPlusTree::tryAcquireSiblingExclusive(std::shared_ptr<BPlusTreeNode> sibling) const {
    if (!sibling) return LatchHandle();
    for (int attempt = 0; attempt < 10000; ++attempt) {
        LatchHandle h = LatchHandle::tryAcquireExclusive(sibling);
        if (h) return h;
        std::this_thread::yield();
    }
    return LatchHandle(); // contended too long; caller falls back to another option
}

std::shared_ptr<BPlusTreeNode> BPlusTree::findAncestorForRelink(
    const std::shared_ptr<BPlusTreeNode>& left_child) const
{
    auto root_snapshot = snapshotRoot();
    if (!root_snapshot || root_snapshot.get() == left_child.get()) return nullptr;

    bool target_is_leaf;
    {
        LatchHandle g(left_child, false);
        target_is_leaf = left_child->is_leaf;
    }

    auto current = root_snapshot;
    for (;;) {
        bool current_is_leaf;
        std::shared_ptr<BPlusTreeNode> rightmost_child;
        {
            LatchHandle g(current, false);
            current_is_leaf = g->is_leaf;
            if (!current_is_leaf && !g->children.empty()) {
                rightmost_child = g->children.back();
            }
        }
        if (current_is_leaf || !rightmost_child) {
            return nullptr;  // reached leaves (or a childless internal node) without matching target level -- corrupt
        }
        bool rightmost_matches_target;
        {
            LatchHandle gc(rightmost_child, false);
            rightmost_matches_target = gc->is_leaf == target_is_leaf;
        }
        if (rightmost_matches_target) {
            return current;  // current's children sit at left_child's level
        }
        current = rightmost_child;
    }
}

void BPlusTree::insert(const Key& key, int page_id, int slot_id, uint64_t txn_id) {
    std::vector<std::shared_ptr<BPlusTreeNode>> dirty;
    DirtyScope dirty_scope(dirty);

    // Shared for the whole descent (see class-level comment in
    // bplustree.hpp): excludes a concurrent structural change
    // (propagateSplit/remove()) from being mid-flight while this
    // traversal is in progress, so it can never reach into a node this
    // descent already holds (or vice versa). Released before calling
    // propagateSplit below -- upgrading shared-to-exclusive in place
    // would itself deadlock against another thread doing the same.
    SharedLatchGuard structure_guard(structure_latch);

    root_guard.lock();
    if (!root) {
        root = std::make_shared<BPlusTreeNode>(true);
    }
    auto root_snapshot = root;
    root_guard.unlock();

    // Phase A: latch-crab down to the target leaf (X throughout), hand-
    // over-hand -- release the parent as soon as the child is latched.
    // Record the internal nodes visited as hints for Phase B.
    std::vector<std::shared_ptr<BPlusTreeNode>> ancestor_hints;
    LatchHandle current(root_snapshot, true);
    current = moveRight(std::move(current), key, true);
    while (!current->is_leaf) {
        ancestor_hints.push_back(current.get());
        int i = 0;
        while (i < static_cast<int>(current->keys.size()) && key >= current->keys[i]) ++i;
        auto child = current->children[i];
        LatchHandle child_handle(child, true);
        current.release();
        current = std::move(child_handle);
        current = moveRight(std::move(current), key, true);
    }

    current->insertInLeaf(key, page_id, slot_id);
    markDirty(current.get());

    std::shared_ptr<BPlusTreeNode> split_left;
    std::shared_ptr<BPlusTreeNode> split_right;
    Key split_key = key;

    if (current->isFull()) {
        auto [sk, new_leaf] = current->splitLeafNode();
        // B-link: link the new sibling in immediately so concurrent
        // readers can find it via high_key/next_leaf even before the
        // parent is fixed up (Phase B, below).
        new_leaf->high_key = current->high_key;
        current->high_key = sk;
        split_left = current.get();
        split_right = new_leaf;
        split_key = sk;
        markDirty(split_right);  // brand-new node -- must be saved regardless
    }

    current.release();
    structure_guard.release();

    if (split_left) {
        propagateSplit(ancestor_hints, split_left, split_right, split_key);
    }

    saveDirty(dirty, txn_id);
}

void BPlusTree::propagateSplit(std::vector<std::shared_ptr<BPlusTreeNode>>& ancestor_hints,
                               std::shared_ptr<BPlusTreeNode> left_child,
                               std::shared_ptr<BPlusTreeNode> right_child,
                               Key separator_key)
{
    // Structural modification (see class-level comment in bplustree.hpp):
    // excludes every traversal (shared) and every other structural
    // change (exclusive) for the whole cascade.
    ExclusiveLatchGuard structure_lock(structure_latch);

    for (;;) {
        // left_child is deliberately NOT latched by us at this point
        // (its own latch was released right after its split, per
        // Lehman & Yao -- that's what lets concurrent access to it
        // proceed without waiting on this parent fix-up). So reading
        // its `keys` needs its own brief latch, same as getParent().
        Key nav_key;
        {
            LatchHandle g(left_child, false);
            nav_key = left_child->keys.front();
        }

        std::shared_ptr<BPlusTreeNode> parent_hint;
        // ancestor_hints was captured during insert()'s own live descent
        // (each entry genuinely alive and latched at the moment it was
        // visited), but insert() releases structure_latch (shared)
        // before calling propagateSplit, which re-acquires it
        // exclusive -- a concurrent remove() can slip into that gap and
        // merge one of these hinted ancestors away. A stale hint can't
        // be trusted blindly: unlike the left_child check further down,
        // a dead parent_hint here would be used directly as `parent`
        // with no further check, silently inserting into a node that's
        // already unlinked from the live tree. So skip dead hints and
        // keep trying older ones before falling back to getParent().
        while (!ancestor_hints.empty()) {
            auto candidate = ancestor_hints.back();
            ancestor_hints.pop_back();
            bool dead;
            {
                LatchHandle g(candidate, false);
                dead = candidate->is_merged_away;
            }
            if (!dead) {
                parent_hint = candidate;
                break;
            }
        }
        if (!parent_hint) {
            parent_hint = getParent(left_child);
        }

        if (!parent_hint) {
            // An unset parent pointer does NOT necessarily mean
            // left_child is the tree root: a brand-new sibling created
            // by splitLeafNode()/splitInternalNode() inherits a
            // placeholder parent (often null) from its sibling until
            // its creator's propagateSplit call gets around to calling
            // setParent() on it. Treating that as "I'm the root" would
            // create a second, competing root that silently discards
            // everything under the real one's other children. So check
            // the only thing that actually answers "is this the root":
            // comparing left_child against the tree's actual root
            // pointer, atomically under root_guard.
            root_guard.lock();
            bool is_root = (root.get() == left_child.get());
            if (is_root) {
                auto new_root = std::make_shared<BPlusTreeNode>(false);
                new_root->keys.push_back(separator_key);
                new_root->children.push_back(left_child);
                new_root->children.push_back(right_child);
                root = new_root;
                root_guard.unlock();
                markDirty(new_root);  // brand-new node -- must be saved regardless
                setParent(left_child, new_root);
                setParent(right_child, new_root);
                return;
            }
            // Not the root -- but if the root is *still a bare leaf*
            // (never yet promoted to an internal node), no propagateSplit
            // call has successfully fixed up any part of this chain, and
            // -- critically -- none ever can while this thread holds
            // structure_latch exclusively: fixing up a parent requires
            // this exact lock. This isn't hypothetical: with the tree
            // still a single leaf, every thread's first few inserts all
            // land on that same leaf (nothing else to route to yet), so
            // multiple threads can each independently split the frontier
            // leaf before any of them gets exclusive access to promote
            // the root. The exclusive lock has no ordering guarantee
            // among waiting writers, so a "downstream" split (of a
            // right-sibling created by an earlier split) can win that
            // race and run before the "upstream" root promotion does --
            // confirmed via gdb: a thread stuck here holding
            // structure_latch while root itself sat unpromoted, verified
            // by inspecting both threads' actual left_child/separator_key
            // values under CPU-constrained concurrent-insert stress.
            //
            // Since this thread already holds the only lock that could
            // ever fix this, do it here instead of waiting: walk root's
            // leaf chain (next_leaf) from the front, collecting every
            // leaf up to and including left_child (they form a
            // contiguous prefix of the chain by construction -- each
            // split only ever appends further right), and promote all of
            // them under one new internal root in a single step.
            // root->is_leaf and, below, each leaf's high_key/next_leaf
            // were last written under that leaf's own per-node latch
            // (by whichever thread split it) -- structure_latch being
            // exclusive rules out concurrent *mutation* of them, but a
            // brief latch on each node read here is what actually
            // establishes the acquire/release pairing needed to see
            // those writes correctly, same defensive reasoning as the
            // left_child read above (see its comment).
            bool root_is_leaf;
            {
                LatchHandle g(root, false);
                root_is_leaf = root->is_leaf;
            }
            if (root_is_leaf) {
                auto new_root = std::make_shared<BPlusTreeNode>(false);
                auto leaf = root;
                bool found = false;
                while (leaf) {
                    new_root->children.push_back(leaf);
                    if (leaf.get() == left_child.get()) {
                        found = true;
                        break;
                    }
                    std::optional<Key> hk;
                    std::shared_ptr<BPlusTreeNode> next;
                    {
                        LatchHandle g(leaf, false);
                        hk = leaf->high_key;
                        next = leaf->next_leaf;
                    }
                    if (!hk.has_value()) break;  // chain ends before reaching left_child
                    new_root->keys.push_back(*hk);
                    leaf = next;
                }
                if (!found) {
                    root_guard.unlock();
                    throw std::runtime_error(
                        "propagateSplit: left_child not reachable via root's leaf chain -- tree structure corrupted");
                }
                root = new_root;
                root_guard.unlock();
                markDirty(new_root);  // brand-new node -- must be saved regardless
                for (auto& child : new_root->children) {
                    setParent(child, new_root);
                }
                parent_hint = new_root;
            } else {
                root_guard.unlock();
                // Root is already internal, but left_child's parent still
                // can't be found via hints/getParent -- can happen several
                // generations deep when multiple threads race to extend
                // the same B-link successor chain, not just directly
                // under the root. findAncestorForRelink() doesn't rely on
                // any cached/inherited pointer at all -- see its own
                // comment for why the rightmost-spine walk always finds
                // the right level.
                parent_hint = findAncestorForRelink(left_child);
                if (!parent_hint) {
                    throw std::runtime_error(
                        "propagateSplit: could not locate left_child's level via root's rightmost spine -- tree structure corrupted");
                }
            }
        }

        // Locate the actual insertion point in two steps. First,
        // moveRight using nav_key (a key already inside left_child, not
        // separator_key itself): separator_key is a brand-new value,
        // and if some ancestor's high_key happened to already equal it
        // exactly, moveRight's `key >= high_key` check would wrongly
        // step one node too far right. A key already established inside
        // left_child can't trigger that false overshoot.
        //
        // But that alone isn't sufficient: left_child's own subtree can
        // legitimately have grown (via further, unrelated inserts)
        // past a boundary that a *different*, concurrent cascade of the
        // same shared ancestor has since imposed -- i.e. our hint can
        // correctly still contain left_child yet no longer be able to
        // accommodate separator_key itself (confirmed via tracing: a
        // delayed fix-up landing after a sibling's cascade already
        // shrank the ancestor's high_key below separator_key). So a
        // second moveRight pass, this time driven by separator_key,
        // keeps walking right until we reach a node that can actually
        // hold it. left_child does not need to be found there -- what
        // has to be correct is where (separator_key, right_child) end
        // up, not that they land adjacent to left_child specifically.
        LatchHandle parent(parent_hint, true);
        parent = moveRight(std::move(parent), nav_key, true);
        parent = moveRight(std::move(parent), separator_key, true);

        // Ensure left_child itself is linked into `parent` before
        // touching right_child at all. "Not found" here does not mean
        // "needs inserting" -- it can also mean left_child is already
        // correctly linked under a DIFFERENT, still-live parent, either
        // because parent_hint for this call resolved to the wrong (but
        // valid) ancestor, or because left_child was already validly
        // absorbed by a concurrent remove()'s merge (which never clears
        // the absorbed node's own fields, so a dead node can still look
        // alive to a naive check). The only reliable signal is
        // left_child's own recorded parent pointer.
        //
        // Deliberately read directly rather than via getParent(): this
        // scope already holds `parent`'s latch exclusively, and
        // left_child's parent pointer can legitimately already equal
        // this exact `parent` (the ordinary not-yet-linked case) --
        // getParent() would then try to latch a node this thread
        // already holds, a guaranteed self-deadlock. Only latch a
        // *different* resolved parent, never the one already held here.
        auto left_it = std::find(parent->children.begin(), parent->children.end(), left_child);
        if (left_it != parent->children.end()) {
            setParent(left_child, parent.get());
        } else {
            std::shared_ptr<BPlusTreeNode> existing_parent;
            {
                LatchHandle g(left_child, false);
                existing_parent = left_child->parent.lock();
            }
            bool already_linked_elsewhere = false;
            if (existing_parent && existing_parent.get() != parent.get().get()) {
                LatchHandle g2(existing_parent, false);
                already_linked_elsewhere = !existing_parent->is_merged_away;
            }
            if (!already_linked_elsewhere) {
                // Genuinely not yet linked anywhere (or its only known
                // parent is dead): insert fresh, using nav_key (a key
                // already inside left_child) as its own separator.
                int left_idx = 0;
                while (left_idx < static_cast<int>(parent->keys.size()) && parent->keys[left_idx] < nav_key) {
                    ++left_idx;
                }
                parent->keys.insert(parent->keys.begin() + left_idx, nav_key);
                parent->children.insert(parent->children.begin() + left_idx + 1, left_child);
                setParent(left_child, parent.get());
            } else {
                // left_child already lives under a different, live
                // parent. right_child is its sibling (created by
                // splitting it), so it belongs under that SAME true
                // parent too -- redirect the rest of this iteration
                // (right_child insertion, fullness/cascade check) to it
                // instead of inserting into the wrong one.
                parent.release();
                parent = LatchHandle(existing_parent, true);
                parent = moveRight(std::move(parent), nav_key, true);
                parent = moveRight(std::move(parent), separator_key, true);
                setParent(left_child, parent.get());
            }
        }

        // Determine right_child's insertion position from separator_key
        // itself via sorted-key comparison, not from left_child's array
        // index: left_child can split more than once before any of its
        // splits get fixed up here (its own latch is released right
        // after each split, by design). If two such splits of the *same*
        // left_child get fixed up out of chronological order by two
        // different threads, "insert right after left_child's current
        // position" would place the second one to arrive *before* the
        // first one's already-inserted sibling, corrupting the sort
        // order. Comparing separator_key against the parent's existing
        // keys always yields the correct position regardless of
        // arrival order.
        //
        // right_child may already be linked too, the mirror of the
        // left_child case above: it may have been split again by
        // another thread and fixed up as *that* cascade's left_child
        // before this call got here. Guard against inserting it twice.
        auto right_it = std::find(parent->children.begin(), parent->children.end(), right_child);
        if (right_it == parent->children.end()) {
            int key_idx = 0;
            while (key_idx < static_cast<int>(parent->keys.size()) && parent->keys[key_idx] < separator_key) {
                ++key_idx;
            }
            parent->keys.insert(parent->keys.begin() + key_idx, separator_key);
            parent->children.insert(parent->children.begin() + key_idx + 1, right_child);
        }
        setParent(right_child, parent.get());
        markDirty(parent.get());

        if (!parent->isFull()) {
            return;
        }

        auto [push_up_key, new_internal] = parent->splitInternalNode();
        for (auto& child : new_internal->children) {
            setParent(child, new_internal);
        }
        new_internal->high_key = parent->high_key;
        new_internal->right_link = parent->right_link;
        parent->high_key = push_up_key;
        parent->right_link = new_internal;
        markDirty(new_internal);  // brand-new node -- must be saved regardless

        left_child = parent.get();
        right_child = new_internal;
        separator_key = push_up_key;
        // `parent` (LatchHandle) releases automatically as this
        // iteration's scope ends.
    }
}

std::optional<RID> BPlusTree::search(const Key& key) {
    SharedLatchGuard structure_guard(structure_latch);
    auto root_snapshot = snapshotRoot();
    if (!root_snapshot) return std::nullopt;

    LatchHandle current(root_snapshot, false);
    current = moveRight(std::move(current), key, false);
    while (!current->is_leaf) {
        int i = 0;
        while (i < static_cast<int>(current->keys.size()) && key >= current->keys[i]) ++i;
        auto child = current->children[i];
        LatchHandle child_handle(child, false);
        current.release();
        current = std::move(child_handle);
        current = moveRight(std::move(current), key, false);
    }
    return current->findInLeaf(key);
}

bool BPlusTree::update(const Key& key, int new_page_id, int new_slot_id, uint64_t txn_id) {
    std::vector<std::shared_ptr<BPlusTreeNode>> dirty;
    DirtyScope dirty_scope(dirty);

    SharedLatchGuard structure_guard(structure_latch);
    auto root_snapshot = snapshotRoot();
    if (!root_snapshot) return false;

    LatchHandle current(root_snapshot, true);
    current = moveRight(std::move(current), key, true);
    while (!current->is_leaf) {
        int i = 0;
        while (i < static_cast<int>(current->keys.size()) && key >= current->keys[i]) ++i;
        auto child = current->children[i];
        LatchHandle child_handle(child, true);
        current.release();
        current = std::move(child_handle);
        current = moveRight(std::move(current), key, true);
    }
    auto node_ptr = current.get();
    bool ok = current->updateInLeaf(key, new_page_id, new_slot_id);
    if (ok) markDirty(node_ptr);
    current.release();
    if (ok) saveDirty(dirty, txn_id);
    return ok;
}

std::vector<std::pair<Key, RID>>
BPlusTree::rangeScan(const Key& low, const Key& high) {
    SharedLatchGuard structure_guard(structure_latch);
    std::vector<std::pair<Key, RID>> results;
    auto root_snapshot = snapshotRoot();
    if (!root_snapshot) return results;

    LatchHandle current(root_snapshot, false);
    current = moveRight(std::move(current), low, false);
    while (!current->is_leaf) {
        int i = 0;
        while (i < static_cast<int>(current->keys.size()) && low >= current->keys[i]) ++i;
        auto child = current->children[i];
        LatchHandle child_handle(child, false);
        current.release();
        current = std::move(child_handle);
        current = moveRight(std::move(current), low, false);
    }

    // Walk the leaf chain, latch-coupling to the right as we go, so a
    // concurrent split never blocks (or is missed by) an in-progress scan.
    while (current) {
        for (size_t i = 0; i < current->keys.size(); i++) {
            if (current->keys[i] >= low && current->keys[i] <= high) {
                results.emplace_back(current->keys[i], current->rids[i]);
            }
            if (current->keys[i] > high) {
                return results;
            }
        }
        auto next = current->next_leaf;
        if (!next) break;
        LatchHandle next_handle(next, false);
        current.release();
        current = std::move(next_handle);
    }

    return results;
}


void BPlusTree::printTree() const {
    SharedLatchGuard structure_guard(structure_latch);
    auto root_snapshot = snapshotRoot();
    if (!root_snapshot) {
        std::cout << "B+ Tree is empty\n"; return;
    }
    std::queue<std::shared_ptr<BPlusTreeNode>> q;
    q.push(root_snapshot);
    int level = 0;

    std::cout << "B+ Tree Structure:\n";
    while (!q.empty()) {
        int size = q.size();
        std::cout << "Level " << level << ": ";
        for (int i = 0; i < size; ++i) {
            auto node = q.front();
            q.pop();

            // Brief S-latch just to safely read this node's fields; not
            // a consistent whole-tree snapshot, which is fine for a
            // debug dump.
            LatchHandle guard(node, false);

            std::cout << "[";
            for (size_t j = 0; j < node->keys.size(); ++j) {
                std::cout << node->keys[j].toString();
                if (j != node->keys.size() - 1) std::cout << ", ";
            }
            std::cout << "] ";

            if (!node->is_leaf) {
                for (auto& child : node->children) {
                    q.push(child);
                }
            }
        }
        std::cout << "\n";
        level++;
    }
    std::cout << "----------------------\n";
}



std::vector<std::pair<Key, RID>> BPlusTree::getAllKeyRIDPairs() const {
    SharedLatchGuard structure_guard(structure_latch);
    std::vector<std::pair<Key, RID>> results;
    auto root_snapshot = snapshotRoot();
    if (!root_snapshot) return results;

    LatchHandle current(root_snapshot, false);
    while (!current->is_leaf) {
        auto child = current->children.front();
        LatchHandle child_handle(child, false);
        current.release();
        current = std::move(child_handle);
    }

    while (current) {
        for (size_t i = 0; i < current->keys.size(); ++i) {
            results.emplace_back(current->keys[i], current->rids[i]);
        }
        auto next = current->next_leaf;
        if (!next) break;
        LatchHandle next_handle(next, false);
        current.release();
        current = std::move(next_handle);
    }
    return results;
}




bool BPlusTree::remove(const Key& key, uint64_t txn_id) {
    std::vector<std::shared_ptr<BPlusTreeNode>> dirty;
    DirtyScope dirty_scope(dirty);

    // Structural modification (see class-level comment in
    // bplustree.hpp): exclusive for the whole call, excluding every
    // traversal and every other structural change -- rebalancing
    // (borrow/merge) mutates shared ancestors the same way a split's
    // fix-up does, and the two (or a merge racing a concurrent plain
    // traversal) is exactly what caused real corruption/deadlocks
    // during development.
    ExclusiveLatchGuard structure_lock(structure_latch);

    // root_snapshot MUST be taken after structure_lock is held, not
    // before: every writer of `root` (propagateSplit's promotions,
    // this function's own root-collapse cases below) needs at least
    // structure_latch shared, so once held exclusively, root is
    // provably stable for the rest of this call. Snapshotting earlier
    // leaves a real TOCTOU window: the root-collapse special case
    // further down uses "root_snapshot was a bare leaf" as a proxy for
    // "I'm deleting the tree's actual current root," but a concurrent
    // propagateSplit could promote a brand-new root in between,
    // demoting root_snapshot to a mere leftmost leaf -- this call,
    // unaware, would then discard the entire newly-promoted subtree.
    auto root_snapshot = snapshotRoot();
    if (!root_snapshot) return false;

    // X-latch the root-to-leaf path and hold it for the entire call
    // rather than releasing early. `path` is remove()'s own verified
    // descent -- deliberately never rediscovered via a node's `parent`
    // pointer, which could be mid-update by a concurrent split. Sibling
    // latches touched while rebalancing are separate, function-local
    // holds (see handleLeafUnderflow/handleInternalUnderflow).
    std::vector<LatchHandle> path;

    LatchHandle current(root_snapshot, true);
    current = moveRight(std::move(current), key, true);
    while (!current->is_leaf) {
        int i = 0;
        while (i < static_cast<int>(current->keys.size()) && key >= current->keys[i]) ++i;
        auto child = current->children[i];
        path.push_back(std::move(current));
        current = LatchHandle(child, true);
        current = moveRight(std::move(current), key, true);
    }
    path.push_back(std::move(current));

    auto leaf = path.back().get();
    auto it = std::find(leaf->keys.begin(), leaf->keys.end(), key);
    if (it == leaf->keys.end()) return false;

    int idx = static_cast<int>(it - leaf->keys.begin());
    leaf->keys.erase(it);
    leaf->rids.erase(leaf->rids.begin() + idx);
    markDirty(leaf);

    // If the deleted key was the first key, update parent separator
    if (idx == 0) {
        if (!leaf->keys.empty()) {
            propagateSeparatorUpdate(path, key, leaf->keys.front());
        } else if (leaf->next_leaf) {
            LatchHandle ng(leaf->next_leaf, false);
            if (!ng->keys.empty()) {
                propagateSeparatorUpdate(path, key, ng->keys.front());
            }
        }
    }

    // Root special case: only nullify the tree if there's genuinely
    // nothing left. path.size()==1 means root itself is a bare leaf
    // (never yet promoted to internal), and this call just emptied it
    // -- but a bare-leaf root can still have a next_leaf continuation:
    // real content from a split that hasn't been promoted into a
    // proper multi-level tree yet (see propagateSplit's root_is_leaf
    // bootstrap). Nullifying root here while next_leaf still points at
    // that continuation would silently orphan it -- the next insert(),
    // finding root null, would bootstrap a brand-new disconnected tree
    // from scratch. Leaving root as the (now empty) leaf instead costs
    // nothing: its high_key/next_leaf still correctly route searches
    // onward via moveRight, and it's cleaned up/promoted normally by
    // whatever future split or merge touches it.
    if (path.size() == 1 && leaf->keys.empty() && !leaf->next_leaf) {
        swapRoot(nullptr);
        path.clear();
        structure_lock.release();
        saveDirty(dirty, txn_id);
        return true;
    }

    // Check for underflow
    if (leaf->isUnderflow()) {
        handleLeafUnderflow(path);
    }

    // Release every latch this call still holds -- including
    // structure_lock -- *before* triggering a save: saveDirty()/
    // IndexManager::saveNode() do their own I/O (and WAL logging) and
    // have no business happening while holding tree latches, and
    // structure_latch itself (shared elsewhere) would self-deadlock if
    // still held here.
    path.clear();
    structure_lock.release();
    saveDirty(dirty, txn_id);
    return true;
}

void BPlusTree::handleLeafUnderflow(std::vector<LatchHandle>& path) {
    if (path.size() < 2) return; // leaf is root; nothing to rebalance against
    auto node = path.back().get();
    auto& parent = path[path.size() - 2];

    auto it = std::find(parent->children.begin(), parent->children.end(), node);
    if (it == parent->children.end()) return;
    int index = static_cast<int>(it - parent->children.begin());

    std::shared_ptr<BPlusTreeNode> left_sibling_ptr =
        (index > 0) ? parent->children[index - 1] : nullptr;
    std::shared_ptr<BPlusTreeNode> right_sibling_ptr =
        (index + 1 < static_cast<int>(parent->children.size())) ? parent->children[index + 1] : nullptr;

    // Try borrowing from left. Acquiring a *left* sibling goes against
    // the left-to-right ordering every traversal (moveRight, descent)
    // follows, so it's a conditional (non-blocking) attempt: a reader
    // or writer could be passing through it via moveRight, wanting to
    // step into `node` next, while we already hold `node` -- blocking
    // here would be a classic AB-BA deadlock. If it's not immediately
    // available, fall through to the right-sibling options instead.
    //
    // Every sibling latch acquired below is a plain local: it's
    // released the moment this function returns (normal C++ scope
    // rules), not held for the rest of remove()'s call -- there's
    // never a reason to keep it any longer than the mutation itself,
    // and holding it longer only widens the window for an unrelated
    // concurrent operation to collide with it.
    if (left_sibling_ptr && left_sibling_ptr->keys.size() > MIN_KEYS) {
        LatchHandle left_sibling = tryAcquireSiblingExclusive(left_sibling_ptr);
        if (left_sibling) {
            node->keys.insert(node->keys.begin(), left_sibling->keys.back());
            node->rids.insert(node->rids.begin(), left_sibling->rids.back());
            left_sibling->keys.pop_back();
            left_sibling->rids.pop_back();
            parent->keys[index - 1] = node->keys.front();
            left_sibling->high_key = node->keys.front();
            markDirty(node); markDirty(left_sibling.get()); markDirty(parent.get());
            return;
        }
    }

    // Try borrowing from right (safe: forward-direction acquisition,
    // same as everyone else).
    if (right_sibling_ptr && right_sibling_ptr->keys.size() > MIN_KEYS) {
        LatchHandle right_sibling(right_sibling_ptr, true);

        node->keys.push_back(right_sibling->keys.front());
        node->rids.push_back(right_sibling->rids.front());
        right_sibling->keys.erase(right_sibling->keys.begin());
        right_sibling->rids.erase(right_sibling->rids.begin());
        parent->keys[index] = right_sibling->keys.front();
        node->high_key = right_sibling->keys.front();
        markDirty(node); markDirty(right_sibling.get()); markDirty(parent.get());
        return;
    }

    // Merge with left sibling (same conditional-acquisition reasoning
    // as the borrow-from-left case above). `node` (this level) is dead
    // once merged away, so pop+release it from `path` immediately --
    // whether or not we go on to cascade -- rather than leaving it
    // latched for the rest of remove()'s call. left_sibling is likewise
    // released *before* any recursive cascade, not held through it --
    // holding a locally-acquired sibling across a (possibly multi-
    // level) recursive cascade was a real, confirmed source of
    // excessive/deadlock-prone latch retention during development.
    if (left_sibling_ptr) {
        LatchHandle left_sibling = tryAcquireSiblingExclusive(left_sibling_ptr);
        if (left_sibling) {
            left_sibling->keys.insert(left_sibling->keys.end(), node->keys.begin(), node->keys.end());
            left_sibling->rids.insert(left_sibling->rids.end(), node->rids.begin(), node->rids.end());
            left_sibling->next_leaf = node->next_leaf;
            left_sibling->high_key = node->high_key;
            node->is_merged_away = true;

            parent->children.erase(parent->children.begin() + index);
            parent->keys.erase(parent->keys.begin() + index - 1);
            path.pop_back(); // release `node`: fully absorbed into left_sibling
            markDirty(left_sibling.get()); markDirty(parent.get());

            if (path.size() == 1 && parent->keys.empty()) {
                // We still hold left_sibling's own latch here, so set
                // its parent directly rather than through setParent()
                // (which would try to re-acquire it: a guaranteed
                // self-deadlock).
                left_sibling->parent.reset();
                auto new_root = left_sibling.get();
                left_sibling.release();
                swapRoot(new_root);
                return;
            }

            left_sibling.release();
            if (parent->isUnderflow()) {
                handleInternalUnderflow(path);
            }
            return;
        }
        // Left sibling contended: if there's no right sibling either,
        // this underflow simply doesn't get fixed on this call -- the
        // tree stays valid (just under-full), and a later remove()
        // will retry once contention clears. Correctness over
        // immediately-perfect balance.
    }

    if (right_sibling_ptr) {
        LatchHandle right_sibling(right_sibling_ptr, true);

        node->keys.insert(node->keys.end(), right_sibling->keys.begin(), right_sibling->keys.end());
        node->rids.insert(node->rids.end(), right_sibling->rids.begin(), right_sibling->rids.end());
        node->next_leaf = right_sibling->next_leaf;
        node->high_key = right_sibling->high_key;
        right_sibling->is_merged_away = true;

        parent->children.erase(parent->children.begin() + index + 1);
        parent->keys.erase(parent->keys.begin() + index);
        right_sibling.release(); // fully absorbed into `node`; done with it
        markDirty(node); markDirty(parent.get());

        if (path.size() == 2 && parent->keys.empty()) {
            // We still hold `node`'s own latch (it's path.back()), so
            // set its parent directly rather than through setParent().
            node->parent.reset();
            path.pop_back();
            swapRoot(node);
            return;
        }

        if (parent->isUnderflow()) {
            path.pop_back();
            handleInternalUnderflow(path);
        }
        return;
    }
}


void BPlusTree::handleInternalUnderflow(std::vector<LatchHandle>& path) {
    if (path.size() < 2) return; // node is root; nothing to rebalance against
    auto node = path.back().get();
    auto& parent = path[path.size() - 2];

    auto it = std::find(parent->children.begin(), parent->children.end(), node);
    if (it == parent->children.end()) return;
    int index = static_cast<int>(it - parent->children.begin());

    std::shared_ptr<BPlusTreeNode> left_sibling_ptr =
        (index > 0) ? parent->children[index - 1] : nullptr;
    std::shared_ptr<BPlusTreeNode> right_sibling_ptr =
        (index + 1 < static_cast<int>(parent->children.size())) ? parent->children[index + 1] : nullptr;

    // Case 1: Borrow from left sibling (conditional acquisition -- see
    // the comment on the equivalent case in handleLeafUnderflow).
    if (left_sibling_ptr && left_sibling_ptr->children.size() > MIN_CHILDREN) {
        LatchHandle left_sibling = tryAcquireSiblingExclusive(left_sibling_ptr);
        if (left_sibling) {
            node->keys.insert(node->keys.begin(), parent->keys[index - 1]);
            parent->keys[index - 1] = left_sibling->keys.back();
            left_sibling->keys.pop_back();
            left_sibling->high_key = parent->keys[index - 1];

            auto moved_child = left_sibling->children.back();
            node->children.insert(node->children.begin(), moved_child);
            setParent(moved_child, node);
            left_sibling->children.pop_back();
            markDirty(node); markDirty(left_sibling.get()); markDirty(parent.get());
            return;
        }
    }

    // Case 2: Borrow from right sibling (safe forward-direction acquisition)
    if (right_sibling_ptr && right_sibling_ptr->children.size() > MIN_CHILDREN) {
        LatchHandle right_sibling(right_sibling_ptr, true);

        node->keys.push_back(parent->keys[index]);
        parent->keys[index] = right_sibling->keys.front();
        right_sibling->keys.erase(right_sibling->keys.begin());
        node->high_key = parent->keys[index];

        auto moved_child = right_sibling->children.front();
        node->children.push_back(moved_child);
        setParent(moved_child, node);
        right_sibling->children.erase(right_sibling->children.begin());
        markDirty(node); markDirty(right_sibling.get()); markDirty(parent.get());
        return;
    }

    // Case 3: Merge with left sibling (conditional acquisition;
    // left_sibling released *before* any recursive cascade -- see the
    // matching comment in handleLeafUnderflow).
    if (left_sibling_ptr) {
        LatchHandle left_sibling = tryAcquireSiblingExclusive(left_sibling_ptr);
        if (left_sibling) {
            left_sibling->keys.push_back(parent->keys[index - 1]);
            left_sibling->keys.insert(left_sibling->keys.end(), node->keys.begin(), node->keys.end());

            left_sibling->children.insert(left_sibling->children.end(), node->children.begin(), node->children.end());
            for (auto& child : node->children) {
                setParent(child, left_sibling.get());
            }
            left_sibling->high_key = node->high_key;
            left_sibling->right_link = node->right_link;
            node->is_merged_away = true;

            parent->keys.erase(parent->keys.begin() + index - 1);
            parent->children.erase(parent->children.begin() + index);
            path.pop_back(); // release `node`: fully absorbed into left_sibling
            markDirty(left_sibling.get()); markDirty(parent.get());

            if (path.size() == 1 && parent->keys.empty()) {
                // Still holding left_sibling's own latch: set its
                // parent directly rather than via setParent() (which
                // would try to re-acquire it -- a self-deadlock).
                left_sibling->parent.reset();
                auto new_root = left_sibling.get();
                left_sibling.release();
                swapRoot(new_root);
                return;
            }

            left_sibling.release();
            if (parent->isUnderflow()) {
                handleInternalUnderflow(path);
            }
            return;
        }
        // Left sibling contended: fall through to right (see the
        // matching comment in handleLeafUnderflow).
    }
    // Case 4: Merge with right sibling
    if (right_sibling_ptr) {
        LatchHandle right_sibling(right_sibling_ptr, true);

        node->keys.push_back(parent->keys[index]);
        node->keys.insert(node->keys.end(), right_sibling->keys.begin(), right_sibling->keys.end());

        node->children.insert(node->children.end(), right_sibling->children.begin(), right_sibling->children.end());
        for (auto& child : right_sibling->children) {
            setParent(child, node);
        }
        node->high_key = right_sibling->high_key;
        node->right_link = right_sibling->right_link;
        right_sibling->is_merged_away = true;

        parent->keys.erase(parent->keys.begin() + index);
        parent->children.erase(parent->children.begin() + index + 1);
        right_sibling.release(); // fully absorbed into `node`; done with it
        markDirty(node); markDirty(parent.get());

        if (path.size() == 2 && parent->keys.empty()) {
            // Still holding `node`'s own latch (it's path.back()): set
            // its parent directly rather than via setParent().
            node->parent.reset();
            path.pop_back();
            swapRoot(node);
            return;
        }

        if (parent->isUnderflow()) {
            path.pop_back();
            handleInternalUnderflow(path);
        }
        return;
    }
}

void BPlusTree::propagateSeparatorUpdate(std::vector<LatchHandle>& path, const Key& old_sep, const Key& new_sep) {
    // path.back() is the node whose first key just changed; search each
    // ancestor (innermost first) for a matching separator. All of these
    // nodes are already X-latched by us (part of `path`), so this can't
    // race with a concurrent split/merge.
    for (int level = static_cast<int>(path.size()) - 2; level >= 0; --level) {
        auto& ancestor = path[level];
        for (size_t i = 0; i < ancestor->keys.size(); ++i) {
            if (ancestor->keys[i] == old_sep) {
                ancestor->keys[i] = new_sep;
                markDirty(ancestor.get());
                // Invariant: children[i]'s high_key mirrors this
                // separator (same bookkeeping the borrow/merge cases
                // below already do for their own parent/sibling pair).
                // children[i] is the *left* sibling of wherever this
                // call's path descended at this level -- descent always
                // steps past keys[i] when key >= keys[i], so it's never
                // something already held in `path`. high_key isn't
                // persisted (see rebuildLinks()), so no markDirty here.
                LatchHandle left_child(ancestor->children[i], true);
                left_child->high_key = new_sep;
                return;
            }
        }
    }
}

void BPlusTree::saveDirty(const std::vector<std::shared_ptr<BPlusTreeNode>>& dirty, uint64_t caller_txn_id) {
    if (!im) return;
    // Serializes IndexManager's actual file I/O across concurrent
    // operations (its fstream isn't safe for concurrent use); not a
    // structural lock; nothing to self-deadlock against. Unlike the old
    // save(), this doesn't need structure_latch: that guarded a full
    // recursive tree walk (node->children) against concurrent shape
    // changes, and saveDirty() no longer walks anything -- `dirty` is
    // already the fixed, concrete list of nodes this operation touched.
    std::lock_guard<std::mutex> io_lock(io_mutex);

    // caller_txn_id == 0 (default): auto-begin/commit here, same as
    // before Phase 5. A real caller_txn_id means everything this call
    // writes -- node saves AND the root-pointer update below -- joins a
    // caller-owned, multi-statement transaction that also covers heap
    // writes; the caller commits/aborts, not us. Begun unconditionally
    // (not just when dirty is non-empty) because setRootNodeID() needs a
    // txn_id too: the root pointer must be undoable in exactly the same
    // transaction as the node data it points at, or a crash between "new
    // root node written" and "old, separately-committed root pointer
    // update" can leave the on-disk root pointing at a node whose
    // creation just got rolled back.
    TransactionManager& txns = im->getTransactionManager();
    bool auto_commit = (caller_txn_id == 0);
    uint64_t txn_id = auto_commit ? txns.begin() : caller_txn_id;

    if (!dirty.empty()) {
        // Assign node_ids to brand-new nodes first, each under its own
        // node's latch: a node can appear in two concurrent operations'
        // dirty sets (e.g. one op mutates it, releases its latches, and
        // a second op touches it again before the first op's saveDirty()
        // runs), so the check-and-assign has to be race-free against
        // another thread doing the same check-and-assign concurrently.
        for (auto& node : dirty) {
            LatchHandle guard(node, true);
            if (node->node_id == -1) {
                node->node_id = im->allocateNodeID();
            }
        }

        // All of this operation's node writes share one WAL transaction,
        // so a crash mid-way through a multi-node structural change (a
        // split cascade, a merge) undoes as a single unit rather than
        // leaving some of the change applied and some not.
        for (auto& node : dirty) {
            // Shared: saveNode() only reads the node to serialize it.
            // Protects against a concurrent operation mutating this same
            // node's fields while we're reading them here (we hold none
            // of our own operation's latches by this point).
            LatchHandle guard(node, false);
            im->saveNode(node, txn_id);
        }
    }

    auto root_snapshot = snapshotRoot();
    im->setRootNodeID(root_snapshot ? root_snapshot->node_id : -1, txn_id);

    if (auto_commit) txns.commit(txn_id);
    im->flush();
}

std::shared_ptr<BPlusTreeNode> BPlusTree::loadRecursive(int node_id) {
    auto node = im->loadNode(node_id);
    if (!node->is_leaf) {
        for (auto& child : node->children) {
            if (child && child->node_id != -1) {
                auto loaded_child = loadRecursive(child->node_id);
                loaded_child->parent = node;
                child = loaded_child;
            }
        }
    }
    return node;
}

void BPlusTree::collectLeavesInOrder(std::shared_ptr<BPlusTreeNode> node,
                                     std::vector<std::shared_ptr<BPlusTreeNode>>& leaves) {
    if (!node) return;
    if (node->is_leaf) {
        leaves.push_back(node);
    } else {
        for (auto& child : node->children) {
            collectLeavesInOrder(child, leaves);
        }
    }
}

void BPlusTree::rebuildLinks() {
    // Level-order traversal: for every node, right_link/high_key mirror
    // the next node at the same depth in left-to-right order, regardless
    // of which parent it belongs to. This is always derivable from the
    // freshly-loaded structure, so nothing about it is persisted to disk.
    if (!root) return;
    std::vector<std::shared_ptr<BPlusTreeNode>> level = { root };
    root->right_link = nullptr;
    root->high_key.reset();

    while (!level.empty()) {
        std::vector<std::shared_ptr<BPlusTreeNode>> next_level;
        for (size_t i = 0; i < level.size(); ++i) {
            auto& node = level[i];
            if (i + 1 < level.size() && !level[i + 1]->keys.empty()) {
                node->high_key = level[i + 1]->keys.front();
                if (!node->is_leaf) {
                    node->right_link = level[i + 1];
                }
            } else {
                node->high_key.reset();
                if (!node->is_leaf) {
                    node->right_link = nullptr;
                }
            }
            if (!node->is_leaf) {
                for (auto& child : node->children) {
                    if (child) next_level.push_back(child);
                }
            }
        }
        level = std::move(next_level);
    }
}

void BPlusTree::load() {
    int root_id = im->getRootNodeID();
    if (root_id < 0) {
        root = std::make_shared<BPlusTreeNode>(true);
        return;
    }
    root = loadRecursive(root_id);

    // Reconstruct next_leaf chain
    std::vector<std::shared_ptr<BPlusTreeNode>> leaves;
    collectLeavesInOrder(root, leaves);
    for (size_t i = 0; i + 1 < leaves.size(); ++i) {
        leaves[i]->next_leaf = leaves[i + 1];
    }
    if (!leaves.empty()) {
        leaves.back()->next_leaf = nullptr;
    }

    rebuildLinks();
}
