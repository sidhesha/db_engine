#include "schema.hpp"
#include <iostream>

int main() {
    Schema schema({{"id", "int"}, {"name", "string"}});
    std::string serialized = schema.serialize();  // "id:int,name:string"
    std::cout << serialized << "\n"; 
    Schema deserialized = Schema::deserialize(serialized);
    std::string serialized2 = deserialized.serialize();  // "id:int,name:string"
    std::cout << serialized2 << "\n";

    return 0;
}