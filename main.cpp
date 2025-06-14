#include "pagemanager.hpp"
#include <iostream>
#include <vector>

int main() {
    PageManager pm("test_db.dat");

    int page_id = pm.allocatePage();
    Page page = pm.readPage(page_id);

    std::vector<std::string> records = {
        "emp_001,Alice,alice@example.com",
        "emp_002,Bob,bob@example.com",
        "emp_003,Charlie,charlie@example.com"
    };

    std::vector<int> slot_ids;
    for (const std::string& r : records) {
        int sid = page.insertRecord(r);
        std::cout << "Inserted: " << r << " in slot " << sid << "\n";
        slot_ids.push_back(sid);
    }

    // Delete record 1
    page.deleteRecord(slot_ids[1]);
    std::cout << "Deleted slot: " << slot_ids[1] << "\n";

    // // Try reading deleted record
    // std::string recovered = page.readRecord(slot_ids[1]);
    // std::cout << "Read deleted record slot " << slot_ids[1] << ": '" << recovered << "'\n";  // Should be empty

    // Fill page with random records
    std::string filler = "x";
    int inserted = 0;
    while (true) {
        // std::cout << inserted << "\n";
        int res = page.insertRecord({filler.begin(), filler.end()});
        if (res == -1) break;
        inserted++;
    }
    std::cout << "Inserted " << inserted << " filler records before hitting limit.\n";

    // Try inserting once more
    int res = page.insertRecord("b");
    if (res == -1) {
        std::cout << "As expected: couldn't insert into full page.\n";
    }
    std::cout << page.readRecord(0) << "\n";
    // std::cout << page.readRecord(1) << "\n"; //deleted above
    std::cout << page.readRecord(2) << "\n";
    std::cout << page.readRecord(3) << "\n";
    std::cout << page.readRecord(665) << "\n"; //last record
    pm.writePage(page);
    return 0;
}
