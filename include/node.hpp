#pragma once
#include <vector>
#include <string>
#include <memory>
#include "constants.hpp"

struct RID {
    int page_id;
    int slot_id;
};

class BPlusTreeNode {
public:
    bool is_leaf;
    int node_id;
    std::vector<std::string> keys;

    // If leaf
    std::vector<RID> rids;
    std::shared_ptr<BPlusTreeNode> next_leaf = nullptr;  // for leaf chaining

    // If internal
    std::vector<std::shared_ptr<BPlusTreeNode>> children;

    std::weak_ptr<BPlusTreeNode> parent; //parent pointer
    
    BPlusTreeNode(bool leaf = true);
    bool isFull() const;
    int findInsertPosition(const std::string& key) const;
    void insertInLeaf(const std::string& key, int page_id, int slot_id);
    void printNode();
    std::pair<std::string, std::shared_ptr<BPlusTreeNode>> splitLeafNode();
    std::pair<std::string, std::shared_ptr<BPlusTreeNode>> splitInternalNode();

    std::vector<char> serialize() const;
    static BPlusTreeNode deserialize(const std::vector<char>& data);
};