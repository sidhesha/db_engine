#pragma once

#include "node.hpp"
#include "indexmanager.hpp"
#include <memory>


class BPlusTree {
public:
    BPlusTree();
    explicit BPlusTree(IndexManager& im);

    void insert(const Key& key, int page_id, int slot_id);
    void printTree() const;
    std::optional<RID> search(const Key& key);
    bool update(const Key& key, int new_page_id, int new_slot_id);
    std::vector<std::pair<Key, RID>> rangeScan(const Key& low, const Key& high);
    std::vector<std::pair<Key, RID>> getAllKeyRIDPairs() const;
    bool remove(const Key& key);

    void save();
    void load();

private:
    std::shared_ptr<BPlusTreeNode> root;
    IndexManager* im;

    void maybeSave();
    void saveRecursive(std::shared_ptr<BPlusTreeNode> node);
    std::shared_ptr<BPlusTreeNode> loadRecursive(int node_id);
    void collectLeavesInOrder(std::shared_ptr<BPlusTreeNode> node,
                              std::vector<std::shared_ptr<BPlusTreeNode>>& leaves);

    void propagateSeparatorUpdate(std::shared_ptr<BPlusTreeNode> child, const Key& old_sep, const Key& new_sep);

    void handleLeafUnderflow(std::shared_ptr<BPlusTreeNode> node);
    void handleInternalUnderflow(std::shared_ptr<BPlusTreeNode> node);
    
    void insertInternal(const Key& key,
                        std::shared_ptr<BPlusTreeNode> left_child,
                        std::shared_ptr<BPlusTreeNode> right_child);

    std::shared_ptr<BPlusTreeNode> findLeafNode(const Key& key);
};