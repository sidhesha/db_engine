#include "record.hpp"
#include <iostream>
#include <cassert>

void printFields(const std::vector<std::string>& fields) {
    for (const auto& field : fields) {
        std::cout << field << " | ";
    }
    std::cout << "\n";
}

int main() {
    std::vector<std::string> original_fields = {
        "123", "John Doe", "john@example.com", "Software Engineer"
    };

    Record rec(original_fields);
    std::cout << "Original fields:\n";
    printFields(rec.getFields());

    std::vector<char> serialized = rec.serialize();
    std::cout << "Serialized size: " << serialized.size() << " bytes\n";

    Record deserialized = Record::deserialize(serialized);
    std::cout << "Deserialized fields:\n";
    printFields(deserialized.getFields());
    std::cout << "Size of deserialized record:" << deserialized.size() << "bytes\n";
    assert(deserialized.size()==serialized.size()); 

    return 0;
}