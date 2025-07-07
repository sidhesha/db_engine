#pragma once

#include "node.hpp"
#include <memory>
#include <string>


class BPlusTree {
public:
    BPlusTree();

    void insert(const Key& key, int page_id, int slot_id);
    void printTree() const;
    std::optional<RID> search(const Key& key);
    bool update(const Key& key, int new_page_id, int new_slot_id);
    std::vector<std::pair<Key, RID>> rangeScan(const Key& low, const Key& high);
    std::vector<std::pair<Key, RID>> getAllKeyRIDPairs() const;
    bool remove(const Key& key);
private:
    std::shared_ptr<BPlusTreeNode> root;

    void insertInternal(const Key& key,
                        std::shared_ptr<BPlusTreeNode> left_child,
                        std::shared_ptr<BPlusTreeNode> right_child);

    std::shared_ptr<BPlusTreeNode> findLeafNode(const Key& key);
};