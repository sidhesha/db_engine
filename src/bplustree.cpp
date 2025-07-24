#include "bplustree.hpp"
#include "constants.hpp"
#include <algorithm>
#include <queue>
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
    if (!root) {
        root = std::make_shared<BPlusTreeNode>(true);
    }
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


// void BPlusTree::printTree() const {
//     std::cout << "B+ Tree Structure:\n";
//     std::vector<std::shared_ptr<BPlusTreeNode>> level = { root };
//     while (!level.empty()) {
//         std::vector<std::shared_ptr<BPlusTreeNode>> next;
//         for (auto& node : level) {
//             node->printNode();
//             if (!node->is_leaf) {
//                 for (auto& child : node->children) {
//                     next.push_back(child);
//                 }
//             }
//         }
//         std::cout << "-----------------\n";
//         level = next;
//     }
// }

void BPlusTree::printTree() const {
    if(root==nullptr){
        std::cout << "B+ Tree is empty\n"; return;
    }
    std::queue<std::shared_ptr<BPlusTreeNode>> q;
    q.push(root);
    int level = 0;

    std::cout << "B+ Tree Structure:\n";
    while (!q.empty()) {
        int size = q.size();
        std::cout << "Level " << level << ": ";
        for (int i = 0; i < size; ++i) {
            auto node = q.front();
            q.pop();

            // Print keys nicely
            std::cout << "[";
            for (size_t j = 0; j < node->keys.size(); ++j) {
                std::cout << node->keys[j].toString();
                if (j != node->keys.size() - 1) std::cout << ", ";
            }
            std::cout << "] ";

            // Add children to queue for next level
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
    std::vector<std::pair<Key, RID>> results;
    auto current = root;
    
    // Navigate to the leftmost leaf
    while (!current->is_leaf) {
        current = current->children.front();
    }
    
    // Traverse all leaves
    while (current) {
        for (size_t i = 0; i < current->keys.size(); ++i) {
            results.emplace_back(current->keys[i], current->rids[i]);
        }
        current = current->next_leaf;
    }
    return results;
}




bool BPlusTree::remove(const Key& key) {
    auto leaf = findLeafNode(key);
    auto it = std::find(leaf->keys.begin(), leaf->keys.end(), key);
    if (it == leaf->keys.end()) return false;

    int idx = it - leaf->keys.begin();
    leaf->keys.erase(it);
    leaf->rids.erase(leaf->rids.begin() + idx);

    // If the deleted key was the first key, update parent separator
    if (idx == 0 && !leaf->keys.empty()) {
        propagateSeparatorUpdate(leaf, key, leaf->keys.front());
    }

    // Root special case
    if (leaf == root && leaf->keys.empty()) {
        root = nullptr;
        return true;
    }

    // Check for underflow
    if (leaf->isUnderflow()) {
        handleLeafUnderflow(leaf);
    }

    return true;
}

void BPlusTree::handleLeafUnderflow(std::shared_ptr<BPlusTreeNode> node) {
    auto parent_ptr = node->parent.lock();
    if (!parent_ptr) return;

    auto parent = parent_ptr;
    auto it = std::find(parent->children.begin(), parent->children.end(), node);
    int index = it - parent->children.begin();

    std::shared_ptr<BPlusTreeNode> left_sibling = (index > 0) ? parent->children[index - 1] : nullptr;
    std::shared_ptr<BPlusTreeNode> right_sibling = (index + 1 < parent->children.size()) ? parent->children[index + 1] : nullptr;

    // Try borrowing from left
    if (left_sibling && left_sibling->keys.size() > MIN_KEYS) {
        node->keys.insert(node->keys.begin(), left_sibling->keys.back());
        node->rids.insert(node->rids.begin(), left_sibling->rids.back());
        left_sibling->keys.pop_back();
        left_sibling->rids.pop_back();
        parent->keys[index - 1] = node->keys.front();
        return;
    }

    // Try borrowing from right
    if (right_sibling && right_sibling->keys.size() > MIN_KEYS) {
        node->keys.push_back(right_sibling->keys.front());
        node->rids.push_back(right_sibling->rids.front());
        right_sibling->keys.erase(right_sibling->keys.begin());
        right_sibling->rids.erase(right_sibling->rids.begin());
        parent->keys[index] = right_sibling->keys.front();
        return;
    }

    // Merge with sibling
    if (left_sibling) {
        left_sibling->keys.insert(left_sibling->keys.end(), node->keys.begin(), node->keys.end());
        left_sibling->rids.insert(left_sibling->rids.end(), node->rids.begin(), node->rids.end());
        left_sibling->next_leaf = node->next_leaf;

        parent->children.erase(parent->children.begin() + index);
        parent->keys.erase(parent->keys.begin() + index - 1);

        if (parent == root && parent->keys.empty()) {
            root = left_sibling;
            root->parent.reset();
            return;
        }

        if (parent->isUnderflow()) {
            handleInternalUnderflow(parent);
        }
    } else if (right_sibling) {
        node->keys.insert(node->keys.end(), right_sibling->keys.begin(), right_sibling->keys.end());
        node->rids.insert(node->rids.end(), right_sibling->rids.begin(), right_sibling->rids.end());
        node->next_leaf = right_sibling->next_leaf;

        parent->children.erase(parent->children.begin() + index + 1);
        parent->keys.erase(parent->keys.begin() + index);

        if (parent == root && parent->keys.empty()) {
            root = node;
            root->parent.reset();
            return;
        }

        if (parent->isUnderflow()) {
            handleInternalUnderflow(parent);
        }
    }
}


void BPlusTree::handleInternalUnderflow(std::shared_ptr<BPlusTreeNode> node) {
    auto parent_ptr = node->parent.lock();
    if (!parent_ptr) return;

    auto parent = parent_ptr;
    auto it = std::find(parent->children.begin(), parent->children.end(), node);
    int index = it - parent->children.begin();

    std::shared_ptr<BPlusTreeNode> left_sibling = (index > 0) ? parent->children[index - 1] : nullptr;
    std::shared_ptr<BPlusTreeNode> right_sibling = (index + 1 < parent->children.size()) ? parent->children[index + 1] : nullptr;

    // Case 1: Borrow from left sibling
    if (left_sibling && left_sibling->children.size() > MIN_CHILDREN) {
        node->keys.insert(node->keys.begin(), parent->keys[index - 1]);
        parent->keys[index - 1] = left_sibling->keys.back();
        left_sibling->keys.pop_back();

        node->children.insert(node->children.begin(), left_sibling->children.back());
        left_sibling->children.back()->parent = node;
        left_sibling->children.pop_back();
        return;
    }

    // Case 2: Borrow from right sibling
    if (right_sibling && right_sibling->children.size() > MIN_CHILDREN) {
        node->keys.push_back(parent->keys[index]);
        parent->keys[index] = right_sibling->keys.front();
        right_sibling->keys.erase(right_sibling->keys.begin());

        node->children.push_back(right_sibling->children.front());
        right_sibling->children.front()->parent = node;
        right_sibling->children.erase(right_sibling->children.begin());
        return;
    }

    // Case 3: Merge with left sibling
    if (left_sibling) {
        left_sibling->keys.push_back(parent->keys[index - 1]);
        left_sibling->keys.insert(left_sibling->keys.end(), node->keys.begin(), node->keys.end());

        left_sibling->children.insert(left_sibling->children.end(), node->children.begin(), node->children.end());
        for (auto& child : node->children) {
            child->parent = left_sibling;
        }

        parent->keys.erase(parent->keys.begin() + index - 1);
        parent->children.erase(parent->children.begin() + index);
    }
    // Case 4: Merge with right sibling
    else if (right_sibling) {
        node->keys.push_back(parent->keys[index]);
        node->keys.insert(node->keys.end(), right_sibling->keys.begin(), right_sibling->keys.end());

        node->children.insert(node->children.end(), right_sibling->children.begin(), right_sibling->children.end());
        for (auto& child : right_sibling->children) {
            child->parent = node;
        }

        parent->keys.erase(parent->keys.begin() + index);
        parent->children.erase(parent->children.begin() + index + 1);
    }

    // Root collapse
    if (parent == root && parent->keys.empty()) {
        root = parent->children[0];
        root->parent.reset();
        return;
    }

    if (parent->isUnderflow()) {
        handleInternalUnderflow(parent);
    }
}

void BPlusTree::propagateSeparatorUpdate(std::shared_ptr<BPlusTreeNode> child, const Key& old_sep, const Key& new_sep) {
    auto parent = child->parent.lock();
    if (!parent) return;
    // Find the separator in the parent that matches old_sep
    for (size_t i = 0; i < parent->keys.size(); ++i) {
        if (parent->keys[i] == old_sep) {
            parent->keys[i] = new_sep;
            // If this separator is also the first key in the parent, propagate up
            return;
        }
    }
    propagateSeparatorUpdate(parent, old_sep, new_sep);
}