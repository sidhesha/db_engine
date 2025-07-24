#include <iostream>
#include <cassert>
#include "catalogmanager.hpp"
#include "pagemanager.hpp"
#include "recordmanager.hpp"
#include "table.hpp"

void test_catalog(){
    const std::string catalog_file = "test_catalog.txt";

    // Cleanup
    std::remove(catalog_file.c_str());

    // Step 1: Create manager and add tables
    {
        CatalogManager cm(catalog_file);

        Schema employee_schema({{"id", "int"}, {"name", "string"}, {"email", "string"}});
        Schema customer_schema({{"customer_id", "int"}, {"address", "string"}});

        cm.createTable("employee", employee_schema);
        cm.createTable("customer", customer_schema);

        // Check that tables are added
        assert(cm.hasTable("employee"));
        assert(cm.hasTable("customer"));

        Schema s = cm.getSchema("employee");
        assert(s.getColumns().size() == 3);
        assert(s.getColumns()[0].name == "id");
        assert(s.getColumns()[1].type == "string");
        assert(s.getColumns()[2].type == "string");
    }

    // Step 2: check persist
    {
        CatalogManager cm2(catalog_file);
        assert(cm2.hasTable("employee"));
        assert(cm2.hasTable("customer"));

        Schema s = cm2.getSchema("customer");
        assert(s.getColumns().size() == 2);
        assert(s.getColumns()[1].name == "address");
        assert(s.getColumns()[1].type == "string");
    }

    std::cout << "All catalog tests passed.\n";
}

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
    std::cout << "Running tests..." << std::endl;
    test_catalog();
    testTableAPI();
    return 0;
}
