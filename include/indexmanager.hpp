// index manager for persisting bplustree on a disk
#pragma once
#include <fstream>
#include <memory>
#include "node.hpp"

class IndexManager {
public:
    explicit IndexManager(const std::string& index_filename);
    ~IndexManager();

    std::shared_ptr<BPlusTreeNode> loadNode(int node_id);
    void saveNode(const std::shared_ptr<BPlusTreeNode>& node);
    int allocateNodeID();
    void flush();

private:
    std::fstream index_file;
    std::string filename;
    int next_node_id;

    void openFile();
    void ensureFileSize(std::size_t size);
};
