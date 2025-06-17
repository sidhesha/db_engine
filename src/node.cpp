#include "node.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>


BPlusTreeNode::BPlusTreeNode(bool leaf)
    : is_leaf(leaf), node_id(-1), next_leaf(nullptr) {}


bool BPlusTreeNode::isFull() const {
    return keys.size() > ORDER-1; //m-1 keys at max
}


int BPlusTreeNode::findInsertPosition(const Key& key) const {
    auto it = std::lower_bound(keys.begin(), keys.end(), key);
    return it - keys.begin();
}

// TODO:Dummy for now
std::vector<char> BPlusTreeNode::serialize() const {
    std::vector<char> buffer;
    return buffer;
}

// TODO:dummy for now
BPlusTreeNode BPlusTreeNode::deserialize(const std::vector<char>& data) {
    
    return BPlusTreeNode();
}

void BPlusTreeNode::insertInLeaf(const Key& key, int page_id, int slot_id){
    int pos = findInsertPosition(key);
    keys.insert(keys.begin() + pos, key);
    rids.insert(rids.begin() + pos, {page_id, slot_id});
}

void BPlusTreeNode::printNode() {
    std::cout << (is_leaf ? "Leaf Node:\n" : "Internal Node:\n");

    if (keys.empty()) {
        std::cout << "Empty node\n";
        return;
    }

    std::cout << "Node ID: " << node_id << "\n";
    std::cout << "Keys: ";
    for (const auto& key : keys) {
        std::cout << key.toString() << " ";
    }
    std::cout << "\n";

    if (is_leaf) {
        for (size_t i = 0; i < keys.size(); ++i) {
            std::cout << "Key: " << keys[i].toString() << " -> (" << rids[i].page_id << ", " << rids[i].slot_id << ")\n";
        }
    } else {
        std::cout << "Children:\n";
        for (size_t i = 0; i < children.size(); ++i) {
            if (children[i]) {
                std::cout << "Child " << i << " -> First key: " 
                          << (children[i]->keys.empty() ? "None" : children[i]->keys.front().toString())
                          << ", Last key: " 
                          << (children[i]->keys.empty() ? "None" : children[i]->keys.back().toString())
                          << "\n";
            } else {
                std::cout << "Child " << i << " -> NULL\n";
            }
        }
    }
}

// Returns the first key of new right node and its pointer
std::pair<Key, std::shared_ptr<BPlusTreeNode>> BPlusTreeNode::splitLeafNode() {
    int mid = keys.size() / 2;

    // new right sibling leaf node
    std::shared_ptr newNode = std::make_shared<BPlusTreeNode>(true);

    newNode->keys.assign(keys.begin() + mid, keys.end());
    newNode->rids.assign(rids.begin() + mid, rids.end());

    // Trim current node
    keys.resize(mid);
    rids.resize(mid);

    // Link the new node
    newNode->next_leaf = this->next_leaf;
    this->next_leaf = newNode;

    newNode->parent = this->parent;

    return {newNode->keys[0], newNode};
}


std::pair<Key, std::shared_ptr<BPlusTreeNode>> BPlusTreeNode::splitInternalNode() {
    int mid = keys.size() / 2;
    Key push_up_key = keys[mid];

    auto new_node = std::make_shared<BPlusTreeNode>(false);

    new_node->keys.assign(keys.begin() + mid + 1, keys.end());
    new_node->children.assign(children.begin() + mid + 1, children.end());

    keys.resize(mid);
    children.resize(mid + 1);

    for (auto& child : new_node->children) {
        if (child) child->parent = new_node;
    }

    return {push_up_key, new_node};
}