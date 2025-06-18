#pragma once
#include <vector>
#include <string>
#include <memory>
#include <optional>
#include "constants.hpp"
#include "key.hpp"

struct RID {
    int page_id;
    int slot_id;
};

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

    std::weak_ptr<BPlusTreeNode> parent; //parent pointer
    
    BPlusTreeNode(bool leaf = true);
    bool isFull() const;
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