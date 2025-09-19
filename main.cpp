#include "bplustree.hpp"
#include <algorithm>
#include <iostream> 
#include <cassert>



void testSerializeDeserialize() {
    auto node = std::make_shared<BPlusTreeNode>(true);
    node->node_id = 1;
    node->is_leaf = true;
    
    node->keys = {Key("asdas"), Key("fdgdug"), Key("saodhd")};
    node->rids = { {1,1}, {1,2}, {1,3} };

    // Serialize
    auto data = node->serialize();
    
    // Deserialize
    BPlusTreeNode restored = BPlusTreeNode::deserialize(data);


    restored.printNode();
    
    assert(restored.keys == node->keys);
    assert(restored.rids.size() == node->rids.size());
    for (size_t i = 0; i < node->rids.size(); i++) {
        assert(restored.rids[i].page_id == node->rids[i].page_id);
        assert(restored.rids[i].slot_id == node->rids[i].slot_id);
    }

    std::cout << "Serialize/Deserialize round-trip test passed" << std::endl;
}



int main() {
    testSerializeDeserialize();
    return 0;
}