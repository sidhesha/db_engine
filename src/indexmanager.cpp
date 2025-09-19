#include "indexmanager.hpp"
#include <iostream>
#include <stdexcept>
#include "constants.hpp"


IndexManager::IndexManager(const std::string& index_filename)
    : filename(index_filename), next_node_id(0) {
    openFile();
}

IndexManager::~IndexManager() {
    flush();
    if (index_file.is_open()) {
        index_file.close();
    }
}



void IndexManager::openFile() {
    // open if existts. in rw mode.
    index_file.open(filename, std::ios::in | std::ios::out | std::ios::binary);

    if (!index_file.is_open()) {
        // here i create file cause it doesn't exist.
        index_file.clear();
        index_file.open(filename, std::ios::out | std::ios::binary);
        index_file.close();

        // reopen in rw mode.
        index_file.open(filename, std::ios::in | std::ios::out | std::ios::binary);

        if (!index_file.is_open()) {
            throw std::runtime_error("Failed to open index file: " + filename);
        }
    }

    index_file.seekg(0, std::ios::end);
    std::size_t file_size = index_file.tellg();

    next_node_id = static_cast<int>(file_size / PAGE_SIZE);
}

int IndexManager::allocateNodeID() {
    return next_node_id++;
}

void IndexManager::ensureFileSize(std::size_t size) {
    index_file.seekp(0, std::ios::end);
    std::size_t current_size = index_file.tellp();

    if (current_size < size) {
        index_file.seekp(0, std::ios::end);

        std::vector<char> zeros(size - current_size, 0);
        index_file.write(zeros.data(), zeros.size());
        index_file.flush();
    }
}

void IndexManager::saveNode(const std::shared_ptr<BPlusTreeNode>& node) {
    auto buffer = node->serialize();

    std::size_t offset = static_cast<std::size_t>(node->node_id) * PAGE_SIZE;
    ensureFileSize(offset + PAGE_SIZE);

    if (buffer.size() > PAGE_SIZE) {
        throw std::runtime_error("Serialized node too large for a page");
    }

    index_file.seekp(offset, std::ios::beg);
    index_file.write(buffer.data(), buffer.size());

    // pad remaining page with zeros
    if (buffer.size() < PAGE_SIZE) {
        std::vector<char> padding(PAGE_SIZE - buffer.size(), 0);
        index_file.write(padding.data(), padding.size());
    }
}

std::shared_ptr<BPlusTreeNode> IndexManager::loadNode(int node_id) {
    std::size_t offset = static_cast<std::size_t>(node_id) * PAGE_SIZE;
    ensureFileSize(offset + PAGE_SIZE);

    index_file.seekg(offset, std::ios::beg);

    std::vector<char> buffer(PAGE_SIZE);
    index_file.read(buffer.data(), PAGE_SIZE);

    auto node = std::make_shared<BPlusTreeNode>();
    *node = BPlusTreeNode::deserialize(buffer);
    node->node_id = node_id;

    return node;
}


void IndexManager::flush() {
    if (index_file.is_open()) {
        index_file.flush();
    }
}