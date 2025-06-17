#include "bplustree.hpp"
#include <iostream>

int main() {
    BPlusTree tree;

    std::vector<std::string> test_keys = {
        "apple", "app", "application", "apex", "apply",
        "bat", "batch", "banana", "band", "bang",
        "cat", "catch", "cater", "category", "car",
        "dog", "doll", "doom", "door", "dot"
    };

    std::cout << "=== Inserting keys ===\n";
    for (int i = 0; i < test_keys.size(); ++i) {
        tree.insert(test_keys[i], i, i);
        std::cout << "Inserted key: " << test_keys[i] << "\n";
    }

    std::cout << "\n=== Tree Structure ===\n";
    tree.printTree();

    return 0;
}
