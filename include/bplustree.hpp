#pragma once

#include "node.hpp"
#include <memory>
#include <string>


class BPlusTree {
public:
    BPlusTree();

    void insert(const std::string& key, int page_id, int slot_id);
    void printTree() const;

private:
    std::shared_ptr<BPlusTreeNode> root;

    void insertInternal(const std::string& key,
                        std::shared_ptr<BPlusTreeNode> left_child,
                        std::shared_ptr<BPlusTreeNode> right_child);

    std::shared_ptr<BPlusTreeNode> findLeafNode(const std::string& key);
};