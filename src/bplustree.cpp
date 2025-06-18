#include "bplustree.hpp"
#include <iostream>


BPlusTree::BPlusTree() {
    root = std::make_shared<BPlusTreeNode>(true);  // root is leaf at start
}

std::shared_ptr<BPlusTreeNode> BPlusTree::findLeafNode(const Key& key) {
    auto current = root;
    while (!current->is_leaf) {
        int i = 0;
        while (i < current->keys.size() && key >= current->keys[i]) {
            ++i;
        }
        current = current->children[i];
    }
    return current;
}

void BPlusTree::insert(const Key& key, int page_id, int slot_id) {
    auto leaf = findLeafNode(key);
    leaf->insertInLeaf(key, page_id, slot_id);

    if (leaf->isFull()) {
        auto [split_key, new_leaf] = leaf->splitLeafNode();

        if (leaf == root) {
            auto new_root = std::make_shared<BPlusTreeNode>(false);
            new_root->keys.push_back(split_key);
            new_root->children.push_back(leaf);
            new_root->children.push_back(new_leaf);

            leaf->parent = new_root;
            new_leaf->parent = new_root;

            root = new_root;
        } else {
            insertInternal(split_key, leaf, new_leaf);
        }
    }
}

void BPlusTree::insertInternal(const Key& key,
                               std::shared_ptr<BPlusTreeNode> left_child,
                               std::shared_ptr<BPlusTreeNode> right_child)
{
    auto parent = left_child->parent.lock();
    if (!parent) return;

    // Insert key and right_child after left_child
    int idx = 0;
    while (idx < parent->children.size() && parent->children[idx] != left_child) {
        ++idx;
    }

    parent->keys.insert(parent->keys.begin() + idx, key);
    parent->children.insert(parent->children.begin() + idx + 1, right_child);
    right_child->parent = parent;

    if (parent->isFull()) {
        auto [push_up_key, new_internal] = parent->splitInternalNode();

        if (parent == root) {
            auto new_root = std::make_shared<BPlusTreeNode>(false);
            new_root->keys.push_back(push_up_key);
            new_root->children.push_back(parent);
            new_root->children.push_back(new_internal);

            parent->parent = new_root;
            new_internal->parent = new_root;
            root = new_root;
        } else {
            insertInternal(push_up_key, parent, new_internal);
        }
    }
}

std::optional<RID> BPlusTree::search(const Key& key) {
    auto leaf = findLeafNode(key);
    return leaf->findInLeaf(key);
}

bool BPlusTree::update(const Key& key, int new_page_id, int new_slot_id) {
    auto leaf = findLeafNode(key);
    return leaf->updateInLeaf(key, new_page_id, new_slot_id);
}

std::vector<std::pair<Key, RID>> 
BPlusTree::rangeScan(const Key& low, const Key& high) {
    std::vector<std::pair<Key, RID>> results;
    auto leaf = findLeafNode(low);

    while (leaf) {
        for (size_t i = 0; i < leaf->keys.size(); i++) {
            if (leaf->keys[i] >= low && leaf->keys[i] <= high) {
                results.emplace_back(leaf->keys[i], leaf->rids[i]);
            }
            if (leaf->keys[i] > high) return results;
        }
        leaf = leaf->next_leaf;
    }

    return results;
}


void BPlusTree::printTree() const {
    std::cout << "B+ Tree Structure:\n";
    std::vector<std::shared_ptr<BPlusTreeNode>> level = { root };
    while (!level.empty()) {
        std::vector<std::shared_ptr<BPlusTreeNode>> next;
        for (auto& node : level) {
            node->printNode();
            if (!node->is_leaf) {
                for (auto& child : node->children) {
                    next.push_back(child);
                }
            }
        }
        std::cout << "-----------------\n";
        level = next;
    }
}
