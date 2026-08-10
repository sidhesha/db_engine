#pragma once
#include <vector>
#include <memory>
#include <optional>
#include "constants.hpp"
#include "key.hpp"
#include "latch.hpp"



class BPlusTreeNode {
public:
    bool is_leaf;
    int node_id;
    std::vector<Key> keys;

    // If leaf
    std::vector<RID> rids;
    std::shared_ptr<BPlusTreeNode> next_leaf = nullptr;  // for leaf chaining

    // If internal
    std::vector<std::shared_ptr<BPlusTreeNode>> children;
    std::shared_ptr<BPlusTreeNode> right_link = nullptr;  // B-link right sibling (internal nodes)

    std::weak_ptr<BPlusTreeNode> parent; //parent pointer

    // B-link high key: exclusive upper bound of keys reachable through
    // this node's subtree. Empty = this is the rightmost node at its
    // level. Lets a latch-crabbing reader/writer that lands on a node
    // mid-split (before the parent has been fixed up) detect it and
    // move right instead of missing the key or blocking.
    std::optional<Key> high_key;

    // Per-node reader/writer latch used by latch-crabbing traversals.
    RWSpinLatch latch;

    // The B-link right-sibling pointer at this node's level: leaves
    // already track this via next_leaf (used elsewhere for range scans),
    // internal nodes use right_link.
    std::shared_ptr<BPlusTreeNode> rightLink() const {
        return is_leaf ? next_leaf : right_link;
    }

    BPlusTreeNode(bool leaf = true);
    bool isFull() const;
    bool isUnderflow() const;
    int findInsertPosition(const Key& key) const;
    void insertInLeaf(const Key& key, int page_id, int slot_id);
    void printNode();
    std::pair<Key, std::shared_ptr<BPlusTreeNode>> splitLeafNode();
    std::pair<Key, std::shared_ptr<BPlusTreeNode>> splitInternalNode();
    std::optional<RID> findInLeaf(const Key& key) const;
    bool updateInLeaf(const Key& key, int new_page_id, int new_slot_id);

    std::vector<char> serialize() const;
    static BPlusTreeNode deserialize(const std::vector<char>& data);
};