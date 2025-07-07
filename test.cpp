#include <iostream>
#include "catalogmanager.hpp"
#include <cassert>

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

int main() {
    test_catalog();
    std::cout << "Running tests..." << std::endl;
    return 0;
}
