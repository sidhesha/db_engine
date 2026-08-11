// index manager for persisting bplustree on a disk
#pragma once
#include <fstream>
#include <memory>
#include "node.hpp"
#include "wal.hpp"

class IndexManager {
public:
    // wal/txns must outlive this IndexManager -- shared with whatever
    // BufferPool the rest of the engine uses, so heap and index writes
    // land in one LSN space / one txn_id space (see bufferpool.hpp).
    IndexManager(const std::string& index_filename, WALWriter& wal, TransactionManager& txns);
    ~IndexManager();

    std::shared_ptr<BPlusTreeNode> loadNode(int node_id);
    // txn_id: the caller's transaction (BPlusTree::saveDirty() owns the
    // begin()/commit() around however many saveNode() calls one tree
    // operation produces, so they're undone/redone as a single unit).
    void saveNode(const std::shared_ptr<BPlusTreeNode>& node, uint64_t txn_id);
    int allocateNodeID();
    void flush();

    bool hasData() const;
    void setRootNodeID(int id);
    int getRootNodeID() const;

    TransactionManager& getTransactionManager() { return txns; }

private:
    std::fstream index_file;
    std::string filename;
    int next_node_id;
    int root_node_id;

    WALWriter& wal;
    TransactionManager& txns;

    void openFile();
    void ensureFileSize(std::size_t size);
    std::size_t nodeOffset(int node_id) const;
    // Reads the current on-disk bytes for node_id's slot, or a
    // zero-filled PAGE_SIZE buffer if that slot has never been written --
    // the WAL before-image for whatever saveNode() is about to write.
    std::vector<char> readExistingBytes(int node_id);
};
