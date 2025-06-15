#include "node.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>


BPlusTreeNode::BPlusTreeNode(bool leaf)
    : is_leaf(leaf), node_id(-1), next_leaf(nullptr) {}


bool BPlusTreeNode::isFull() const {
    return keys.size() >= MAX_KEYS;
}


int BPlusTreeNode::findInsertPosition(const std::string& key) const {
    auto it = std::lower_bound(keys.begin(), keys.end(), key);
    return it - keys.begin();
}

// Dummy for now
std::vector<char> BPlusTreeNode::serialize() const {
    std::vector<char> buffer;
    return buffer;
}

// dummy for now
BPlusTreeNode BPlusTreeNode::deserialize(const std::vector<char>& data) {
    
    return BPlusTreeNode();
}

void BPlusTreeNode::insertInLeaf(const std::string& key, int page_id, int slot_id){
    int pos = findInsertPosition(key);
    keys.insert(keys.begin() + pos, key);
    rids.insert(rids.begin() + pos, {page_id, slot_id});
}

void BPlusTreeNode::printNode() {
    std::cout << (is_leaf ? "Leaf Node:\n" : "Internal Node:\n");
    for (size_t i = 0; i < keys.size(); ++i) {
        if (is_leaf) {
            std::cout << "Key: " << keys[i];
            std::cout << " -> (" << rids[i].page_id << ", " << rids[i].slot_id << ")";
        } else {
            std::cout << " -> Child: " << children[i + 1]; //child 0 is the node itself
        }
        std::cout << "\n";
    }
}

// Returns the first key of new right node and its pointer
std::pair<std::string, std::shared_ptr<BPlusTreeNode>> BPlusTreeNode::splitLeafNode() {
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

    return {newNode->keys[0], newNode};
}

