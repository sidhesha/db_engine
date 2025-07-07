#include "pagemanager.hpp"
#include "recordmanager.hpp"
#include "table.hpp"
#include <iostream>
#include <cassert>

void testTableAPI() {
    PageManager page_manager("dbfile.db");
    RecordManager record_manager(page_manager);

    Schema schema = {{{"id", "int"}, {"name", "string"}, {"email", "string"}}};
    Table employee_table("employee", schema, page_manager, record_manager);

    employee_table.insert({"1", "Alice", "alice@example.com"});
    employee_table.insert({"2", "Bob", "bob@example.com"});
    employee_table.insert({"3", "Charlie", "charlie@example.com"});

    std::cout << "Printing all records:\n";
    employee_table.printAll();

    std::cout << "\nSearch for key 2:\n";
    auto rec = employee_table.getByKey("2");
    if (rec.has_value()) {
        for (const auto& val : rec.value().getFields()) {
            std::cout << val << " ";
        }
        std::cout << "\n";
    }

    std::cout << "\nDeleting key 2...\n";
    employee_table.deleteByKey("2");

    std::cout << "\nAfter deletion:\n";
    employee_table.printAll();
}

int main() {
    testTableAPI();
    return 0;
}