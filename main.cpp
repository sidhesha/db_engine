#include "bplustree.hpp"
#include <algorithm>
#include <iostream>

void testDeletionStress() {
    BPlusTree tree;

    // Insert 20 keys
    for (int i = 1; i <= 20; ++i) {
        tree.insert(int(i), i, 0);
    }

    std::cout << "\nInitial tree after insertion:\n";
    tree.printTree();

    // tree.remove(7);
    // tree.printTree();
    // Delete a few keys one by one
    std::vector<int> to_delete = {7, 1, 20, 10, 3, 4, 6, 5, 2, 8, 9, 11};
    for (int key : to_delete) {
        std::cout << "\n--- Deleting key: " << key << " ---\n";
        tree.remove(key);
        tree.printTree();
    }

    // Final deletes to trigger root collapse
    for (int key = 12; key <= 19; ++key) {
        std::cout << "\n--- Deleting key: " << key << " ---\n";
        tree.remove(key);
        tree.printTree();
    }
    tree.insert(int(3), 0, 0);
    std::cout << "\nFinal tree (should be empty or root-only):\n";
    tree.printTree();

}


int main() {
    testDeletionStress();
    return 0;
}