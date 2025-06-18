#include "bplustree.hpp"
#include <iostream>

int main() {
    BPlusTree tree;

    std::vector<std::string> test_keys = {
        "door", "dot",
        "catch", "cater", "apply",
        "band", "bang", "cat","bat", "batch", "banana", 
        "dog", "doll", "doom", "apple", "app", "application", "apex",
        "category", "car"
    };

    // std::vector<int> test_keys = {
    //     1,6,3,2,5,10,25,100,56,23,11
    // };

    std::cout << "=== Inserting keys ===\n";
    for (int i = 0; i < test_keys.size(); ++i) {
        tree.insert(test_keys[i], i, i);
        std::cout << "Inserted key: " << test_keys[i] << "\n";
    }

    std::cout << "\n=== Tree Structure ===\n";
    tree.printTree();
    tree.update(Key("car"),10,20);
    auto result = tree.search(Key("car"));
    if (result) {
        std::cout << "Found: page=" << result->page_id << ", slot=" << result->slot_id << "\n";
    } else {
        std::cout << "Key not found.\n";
    }

    // auto range_result = tree.rangeScan(Key("a"),Key("z"));
    // auto range_result = tree.rangeScan(Key("batch"),Key("-"));
    // auto range_result = tree.rangeScan(Key("-"),Key("cattox"));
    auto range_result = tree.rangeScan(Key("batch"),Key("cat"));

    for(auto [k,r]:range_result){
        std::cout << "Key=" << k.toString() << " (page_id=" << r.page_id << " ,slot_id=" << r.slot_id << ")\n";
    }

    return 0;
}
