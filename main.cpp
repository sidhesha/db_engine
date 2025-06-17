#include "bplustree.hpp"
#include <iostream>

int main() {
    BPlusTree tree;

    // std::vector<std::string> test_keys = {
    //     "door", "dot",
    //     "catch", "cater", "apply",
    //     "band", "bang", "cat","bat", "batch", "banana", 
    //     "dog", "doll", "doom", "apple", "app", "application", "apex",
    //     "category", "car"
    // };

    std::vector<int> test_keys = {
        1,6,3,2,5,10,25,100,56,23,11
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
