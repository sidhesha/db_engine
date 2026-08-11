#include "indexmanager.hpp"
#include <iostream>
#include <stdexcept>
#include "constants.hpp"


IndexManager::IndexManager(const std::string& index_filename, WALWriter& wal, TransactionManager& txns)
    : filename(index_filename), next_node_id(0), root_node_id(-1), wal(wal), txns(txns) {
    openFile();
    // Read header (root_node_id) from offset 0 if file has data
    index_file.seekg(0, std::ios::beg);
    if (index_file.good()) {
        index_file.read(reinterpret_cast<char*>(&root_node_id), sizeof(int));
        if (!index_file.good()) {
            root_node_id = -1;
            index_file.clear(); // stream may have failed on empty file
        }
    }
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

    if (file_size > PAGE_SIZE) {
        next_node_id = static_cast<int>((file_size - PAGE_SIZE) / PAGE_SIZE);
    } else {
        next_node_id = 0;
    }
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

std::vector<char> IndexManager::readExistingBytes(int node_id) {
    std::size_t offset = nodeOffset(node_id);

    index_file.seekg(0, std::ios::end);
    std::size_t file_size = index_file.tellg();
    if (file_size < offset + PAGE_SIZE) {
        // This node_id's slot has never been written -- the WAL
        // before-image is "nothing there yet", i.e. a zero-filled page.
        return std::vector<char>(PAGE_SIZE, 0);
    }

    std::vector<char> buffer(PAGE_SIZE);
    index_file.seekg(offset, std::ios::beg);
    index_file.read(buffer.data(), PAGE_SIZE);
    if (!index_file) {
        index_file.clear();
        throw std::runtime_error(
            "IndexManager::readExistingBytes: failed to read node " + std::to_string(node_id));
    }
    return buffer;
}

void IndexManager::saveNode(const std::shared_ptr<BPlusTreeNode>& node, uint64_t txn_id) {
    std::vector<char> old_data = readExistingBytes(node->node_id);
    std::vector<char> new_data = node->serialize();

    if (new_data.size() > PAGE_SIZE) {
        throw std::runtime_error("Serialized node too large for a page");
    }

    uint64_t lsn = txns.appendRecord(txn_id, WALRecordType::UPDATE, WALStore::INDEX,
                                      node->node_id, old_data, new_data);
    // WAL rule: the record covering this node's change must be durable
    // before the node itself reaches disk. Unlike BufferPool, IndexManager
    // has no deferred write-back stage to hook this into -- the write
    // below happens immediately -- so it's enforced right here.
    wal.flushUpTo(lsn);

    node->lsn = lsn;
    new_data = node->serialize();  // re-serialize so the on-disk copy carries its own LSN

    std::size_t offset = nodeOffset(node->node_id);
    ensureFileSize(offset + PAGE_SIZE);

    index_file.seekp(offset, std::ios::beg);
    index_file.write(new_data.data(), new_data.size());

    // pad remaining page with zeros
    if (new_data.size() < PAGE_SIZE) {
        std::vector<char> padding(PAGE_SIZE - new_data.size(), 0);
        index_file.write(padding.data(), padding.size());
    }
}

std::shared_ptr<BPlusTreeNode> IndexManager::loadNode(int node_id) {
    std::size_t offset = nodeOffset(node_id);

    // Don't grow the file here: unlike saveNode(), a load must never
    // silently manufacture data for a node_id that was never saved.
    // Reading past the end would return a zero-filled page that
    // deserializes into a bogus-but-valid-looking empty node instead of
    // surfacing the corruption/bug loudly.
    index_file.seekg(0, std::ios::end);
    std::size_t file_size = index_file.tellg();
    if (file_size < offset + PAGE_SIZE) {
        throw std::runtime_error(
            "IndexManager::loadNode: node_id " + std::to_string(node_id) +
            " was never saved (index file too small)");
    }

    index_file.seekg(offset, std::ios::beg);

    std::vector<char> buffer(PAGE_SIZE);
    index_file.read(buffer.data(), PAGE_SIZE);
    if (!index_file) {
        index_file.clear();
        throw std::runtime_error(
            "IndexManager::loadNode: failed to read node " + std::to_string(node_id));
    }

    auto node = std::make_shared<BPlusTreeNode>();
    *node = BPlusTreeNode::deserialize(buffer);
    node->node_id = node_id;

    return node;
}

bool IndexManager::hasData() const {
    return root_node_id >= 0;
}

void IndexManager::setRootNodeID(int id) {
    root_node_id = id;
    index_file.seekp(0, std::ios::beg);
    index_file.write(reinterpret_cast<const char*>(&root_node_id), sizeof(int));
    index_file.flush();
}

int IndexManager::getRootNodeID() const {
    return root_node_id;
}

std::size_t IndexManager::nodeOffset(int node_id) const {
    return PAGE_SIZE + static_cast<std::size_t>(node_id) * PAGE_SIZE;
}


void IndexManager::flush() {
    if (index_file.is_open()) {
        index_file.flush();
    }
}
