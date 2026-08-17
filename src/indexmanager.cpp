#include "indexmanager.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include "constants.hpp"

namespace {
// The root-pointer header lives in the same PAGE_SIZE-sized slot
// preceding node 0 that nodeOffset()'s formula already reserves for it
// (nodeOffset(-1) == PAGE_SIZE + (-1)*PAGE_SIZE == 0). Logging its
// changes as a WALStore::INDEX UPDATE record with this reserved page_id
// -- instead of writing the header directly to disk, which is what this
// code used to do -- means RecoveryManager's existing generic redo/undo
// (built entirely around "PAGE_SIZE-shaped blob with an LSN at a fixed
// offset") protects it exactly like a real node, no special-casing
// needed anywhere in RecoveryManager itself.
constexpr int32_t ROOT_POINTER_PAGE_ID = -1;
// Matches BPlusTreeNode's own on-disk header LSN offset (INDEX_LSN_OFFSET
// in recoverymanager.cpp) so the same lsnFieldOffset(WALStore::INDEX)
// RecoveryManager already uses for every other index page works here too.
constexpr std::size_t ROOT_LSN_OFFSET = 12;
}  // namespace

IndexManager::IndexManager(const std::string& index_filename, WALWriter& wal, TransactionManager& txns)
    : filename(index_filename), next_node_id(0), root_node_id(-1), wal(wal), txns(txns) {
    openFile();
    std::vector<char> header = currentHeaderBytes();
    std::memcpy(&root_node_id, header.data(), sizeof(root_node_id));

    // Materialize a proper PAGE_SIZE header slot up front if one doesn't
    // exist yet. Without this, a LATER, unrelated ensureFileSize() call
    // (e.g. from the very first saveNode(), growing the file out to
    // cover node 0's slot at [PAGE_SIZE, 2*PAGE_SIZE)) zero-fills from
    // the file's *current end* forward -- which, on a brand-new file,
    // means straight through this header region too, as a side effect,
    // before setRootNodeID() ever gets a chance to write its own value
    // here. That accidental zero-fill is indistinguishable from a
    // legitimate root_node_id == 0, permanently losing the "-1 == no
    // root yet" sentinel and silently defeating setRootNodeID()'s
    // change-detection (old reads back as 0 the same as new, so it
    // thinks nothing changed and never logs the WAL record at all).
    index_file.seekg(0, std::ios::end);
    if (static_cast<std::size_t>(index_file.tellg()) < static_cast<std::size_t>(PAGE_SIZE)) {
        index_file.seekp(0, std::ios::beg);
        index_file.write(header.data(), header.size());
        index_file.flush();
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

std::vector<char> IndexManager::currentHeaderBytes() {
    index_file.seekg(0, std::ios::end);
    std::size_t file_size = index_file.tellg();
    if (file_size < static_cast<std::size_t>(PAGE_SIZE)) {
        // Never written -- root_node_id must read back as -1, not 0
        // (0 is a valid real node_id once the tree has a root).
        std::vector<char> buffer(PAGE_SIZE, 0);
        int32_t unset = -1;
        std::memcpy(buffer.data(), &unset, sizeof(unset));
        return buffer;
    }

    std::vector<char> buffer(PAGE_SIZE);
    index_file.seekg(0, std::ios::beg);
    index_file.read(buffer.data(), PAGE_SIZE);
    if (!index_file) {
        index_file.clear();
        throw std::runtime_error("IndexManager: failed to read root-pointer header");
    }
    return buffer;
}

void IndexManager::setRootNodeID(int id, uint64_t txn_id) {
    std::vector<char> old_header = currentHeaderBytes();
    int32_t old_id;
    std::memcpy(&old_id, old_header.data(), sizeof(old_id));

    root_node_id = id;
    if (old_id == id) {
        return;  // no actual change -- nothing to log or write
    }

    std::vector<char> new_header = old_header;
    int32_t new_id = id;
    std::memcpy(new_header.data(), &new_id, sizeof(new_id));

    uint64_t lsn = txns.appendRecord(txn_id, WALRecordType::UPDATE, WALStore::INDEX,
                                      ROOT_POINTER_PAGE_ID, old_header, new_header);
    // WAL rule: durable before the header itself reaches disk.
    wal.flushUpTo(lsn);
    std::memcpy(new_header.data() + ROOT_LSN_OFFSET, &lsn, sizeof(lsn));

    ensureFileSize(PAGE_SIZE);
    index_file.seekp(0, std::ios::beg);
    index_file.write(new_header.data(), new_header.size());
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
