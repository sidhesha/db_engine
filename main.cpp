#include "node.hpp"
#include <iostream>
#include <vector>

int main() {
    std::shared_ptr leaf = std::make_shared<BPlusTreeNode>(true);  // is_leaf = true

    std::vector<std::string> test_keys = {"frank", "alex", "bob", "eve", "david","carol" };
    int page_id = 1;

    for (int i = 0; i < test_keys.size(); ++i) {
        leaf->insertInLeaf(test_keys[i], page_id, i);
        
        if (leaf->isFull()) {
            auto [split_key, new_node] = leaf->splitLeafNode();

            std::cout << "Split occurred at key: " << split_key << "\n";
            std::cout << "Left node:\n";
            leaf->printNode();
            std::cout << "Right node:\n";
            new_node->printNode();
        }
    }
    return 0;
}
