#pragma once

#include "record.hpp"
#include "pagemanager.hpp"
#include "constants.hpp"

class RecordManager {
public:
    RecordManager(PageManager& page_manager);

    RID insertRecord(const Record& record);
    Record readRecord(const RID& rid);
    bool deleteRecord(const RID& rid);

private:
    PageManager& page_manager;
};
