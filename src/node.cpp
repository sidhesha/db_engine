#include "node.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include "constants.hpp"


BPlusTreeNode::BPlusTreeNode(bool leaf)
    : is_leaf(leaf), node_id(-1), next_leaf(nullptr) {}


bool BPlusTreeNode::isFull() const {
    return keys.size() > ORDER-1; //m-1 keys at max
}

bool BPlusTreeNode::isUnderflow() const {
    return keys.size() < (ORDER - 1) / 2;
}



int BPlusTreeNode::findInsertPosition(const Key& key) const {
    auto it = std::lower_bound(keys.begin(), keys.end(), key);
    return it - keys.begin();
}

std::vector<char> BPlusTreeNode::serialize() const {
    std::vector<char> buffer(PAGE_SIZE, 0); // fixed page size
    std::size_t offset = 0;

    auto write_int = [&](int value) {
        std::memcpy(buffer.data() + offset, &value, sizeof(int));
        offset += sizeof(int);
    };

    auto write_bool = [&](bool value) {
        char b = value ? 1 : 0;
        std::memcpy(buffer.data() + offset, &b, sizeof(char));
        offset += 4; // align to 4 bytes
    };

    // Header
    // node_id + is_leaf + num_keys + padding = 32 bytes
    write_int(node_id); 
    write_bool(is_leaf); 
    write_int(static_cast<int>(keys.size()));

    // padding up to 32 bytes
    offset = 32;

    // Keys
    for (const auto& key : keys) {
        int type_tag = static_cast<int>(key.getType());
        write_int(type_tag);

        switch (key.getType()) {
            case KeyTypeTag::INTEGER:
                write_int(std::get<int>(key.value));
                break;
            case KeyTypeTag::FLOAT: {
                float f = std::get<float>(key.value);
                std::memcpy(buffer.data() + offset, &f, sizeof(float));
                offset += sizeof(float);
                break;
            }
            case KeyTypeTag::STRING: {
                std::string str = std::get<std::string>(key.value);
                int len = static_cast<int>(str.size());
                write_int(len);
                std::memcpy(buffer.data() + offset, str.data(), len);
                offset += len;
                break;
            }
        }
    }

    // Payload
    if (is_leaf) {
        for (const auto& rid : rids) {
            write_int(rid.page_id);
            write_int(rid.slot_id);
        }
    } else {
        for (const auto& child : children) {
            int child_id = child ? child->node_id : -1;
            write_int(child_id);
        }
    }

    return buffer;
}

BPlusTreeNode BPlusTreeNode::deserialize(const std::vector<char>& data) {
    if (data.size() < PAGE_SIZE) {
        throw std::runtime_error("Invalid page size for node deserialization");
    }

    BPlusTreeNode node;

    std::size_t offset = 0;

    auto read_int = [&](int& value) {
        std::memcpy(&value, data.data() + offset, sizeof(int));
        offset += sizeof(int);
    };

    auto read_bool = [&](bool& value) {
        char b;
        std::memcpy(&b, data.data() + offset, sizeof(char));
        offset += 4; // aligned
        value = (b != 0);
    };

    // Header
    read_int(node.node_id);
    read_bool(node.is_leaf);
    int num_keys;
    read_int(num_keys);

    offset = 32;

    // Keys
    node.keys.clear();
    for (int i = 0; i < num_keys; i++) {
        Key key;
        int type_tag;
        read_int(type_tag);
        switch (type_tag) {
            case 0: // INTEGER
                int int_val;
                read_int(int_val);
                key.value = int_val;
                break;
            case 1: // FLOAT
                float f;
                std::memcpy(&f, data.data() + offset, sizeof(float));
                offset += sizeof(float);
                key.value = f;
                break;
            case 2: // STRING
                int len;
                read_int(len);
                std::string str(len, '\0');
                std::memcpy(str.data(), data.data() + offset, len);
                offset += len;
                key.value = str;
                break;
        }
        node.keys.push_back(key);
    }

    // Payload
    if (node.is_leaf) {
        node.rids.clear();
        for (int i = 0; i < num_keys; i++) {
            RID rid;
            read_int(rid.page_id);
            read_int(rid.slot_id);
            node.rids.push_back(rid);
        }
    } else {
        node.children.clear();
        for (int i = 0; i < num_keys + 1; i++) {
            int child_id;
            read_int(child_id);
            if (child_id != -1) {
                // TODO: will be re-linked by IndexManager later
                auto child = std::make_shared<BPlusTreeNode>();
                child->node_id = child_id;
                node.children.push_back(child);
            } else {
                node.children.push_back(nullptr);
            }
        }
    }

    return node;
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


std::optional<RID> BPlusTreeNode::findInLeaf(const Key& key) const {
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == key) {
            return rids[i];
        }
    }
    return std::nullopt;
}

bool BPlusTreeNode::updateInLeaf(const Key& key, int new_page_id, int new_slot_id) {
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == key) {
            rids[i] = {new_page_id, new_slot_id};
            return true;
        }
    }
    return false;
}

