#include "recordmanager.hpp"
#include <iostream>
#include <cassert>
#include <random>

void printFields(const std::vector<std::string>& fields) {
    for (const auto& field : fields) {
        std::cout << field << " | ";
    }
    std::cout << "\n";
}

int main() {
    // std::vector<std::string> original_fields = {
    //     "123", "John Doe", "john@example.com", "Software Engineer"
    // };

    // Record rec(original_fields);
    // std::cout << "Original fields:\n";
    // printFields(rec.getFields());

    // std::vector<char> serialized = rec.serialize();
    // std::cout << "Serialized size: " << serialized.size() << " bytes\n";

    // Record deserialized = Record::deserialize(serialized);
    // std::cout << "Deserialized fields:\n";
    // printFields(deserialized.getFields());
    // std::cout << "Size of deserialized record:" << deserialized.size() << "bytes\n";
    // assert(deserialized.size()==serialized.size()); 
    PageManager pm("data.db");
    RecordManager rm(pm);

    // Record r1({"Alice", "alice@example.com"});
    // Record r2({"Bob", "bob@example.com"});

    // RID rid1 = rm.insertRecord(r1);
    // RID rid2 = rm.insertRecord(r2);

    // std::cout << "Inserted RIDs: " << rid1.page_id << "," << rid1.slot_id << " and " 
    //         << rid2.page_id << "," << rid2.slot_id << "\n";

    // Record fetched = rm.readRecord(rid1);
    // std::cout << "Fetched record:\n";
    // printFields(fetched.getFields());

    // rm.deleteRecord(rid1);
    // try {
    //     Record should_fail = rm.readRecord(rid1);
    //     std::cout << "Read after delete (unexpected): ";
    //     printFields(should_fail.getFields());
    // } catch (const std::exception& e) {
    //     std::cout << "Expected failure on read after delete: " << e.what() << "\n";
    // }

    // std::vector<RID> rids;
    // for (int i = 0; i < 500; ++i) {
    //     std::string name = "User" + std::to_string(i);
    //     std::string email = "user" + std::to_string(i) + "@example.com";
    //     Record rec({name, email});
    //     rids.push_back(rm.insertRecord(rec));
    // }
    // std::cout << "Inserted 500 records.\n";


    // std::random_device rd;
    // std::mt19937 gen(rd());
    // std::uniform_int_distribution<> dis(0, rids.size() - 1);
    // for (int i = 0; i < 20; ++i) {
    //     int index = dis(gen);
    //     rm.deleteRecord(rids[index]);
    //     std::cout << "Deleted record at index " << index << "\n";
    // }

    // for (int i = 0; i < 50; ++i) {
    //     int index = dis(gen);
    //     try {
    //         Record r = rm.readRecord(rids[index]);
    //         std::cout << "Fetched record:\n";
    //         printFields(r.getFields());
    //     } catch (const std::exception& e) {
    //         std::cout << "Correctly failed to read deleted record at index " << index << "\n";
    //     }
    // }

    std::string long_str(4062, 'X');  // Large string so that it becomes page size
    Record big_rec({"BigRecord", long_str});

    try {
        RID big_rid = rm.insertRecord(big_rec);
        Record r = rm.readRecord(big_rid);
        printFields(r.getFields());
    } catch (const std::exception& e) {
        std::cout << "Failed to insert large record: " << e.what() << "\n";
    }
    return 0;
}