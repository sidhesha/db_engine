#include "../include/page.hpp"
#include <iostream>
#include <cstring>
Page::Page(uint32_t page_id): page_id(page_id), num_slots(0), free_space_offset(PAGE_SIZE)
{
    data.resize(PAGE_SIZE, 0);

    // memory for header
    std::memcpy(&data[0], &page_id, sizeof(page_id)); 
    std::memcpy(&data[4], &num_slots, sizeof(num_slots));
    std::memcpy(&data[6], &free_space_offset, sizeof(free_space_offset));
    // std::cout << "page created with id:" << this->page_id << "\n"; 
}


void Page::updateHeaderToData() {
    std::memcpy(&data[0], &page_id, sizeof(page_id));
    std::memcpy(&data[4], &num_slots, sizeof(num_slots));
    std::memcpy(&data[6], &free_space_offset, sizeof(free_space_offset));
}

void Page::updateSlotDirectory() {
    for (int i = 0; i < slot_directory.size(); ++i) {
        int slot_offset = PAGE_HEADER_SIZE + i * SLOT_ENTRY_SIZE;

        const SlotEntry& entry = slot_directory[i];
        std::memcpy(&data[slot_offset], &entry.offset, sizeof(entry.offset));
        std::memcpy(&data[slot_offset + 2], &entry.length, sizeof(entry.length));
        std::memcpy(&data[slot_offset + 4], &entry.is_active, sizeof(entry.is_active));
    }
}

std::vector<char> Page::serialize() {
    
    Page::updateHeaderToData();
    Page::updateSlotDirectory();
    std::vector<char> buffer(PAGE_SIZE);

    std::memcpy(buffer.data(), data.data(), PAGE_SIZE);
    
    return buffer;
}

Page Page::deserialize(const std::vector<char> raw) {

    uint32_t page_id;
    uint16_t num_slots, free_space_offset;

    std::memcpy(&page_id, &raw[0], sizeof(page_id));
    std::memcpy(&num_slots, &raw[4], sizeof(num_slots));
    std::memcpy(&free_space_offset, &raw[6], sizeof(free_space_offset));

    Page page(page_id);
    page.num_slots = num_slots;
    page.free_space_offset = free_space_offset;

    // Copy raw data buffer into page.data
    std::memcpy(page.data.data(), raw.data(), PAGE_SIZE);


    page.slot_directory.clear();
    for (int i = 0; i < num_slots; ++i) {
        int slot_offset = PAGE_HEADER_SIZE + (i) * SLOT_ENTRY_SIZE;


        SlotEntry entry;
        std::memcpy(&entry.offset, &raw[slot_offset], sizeof(entry.offset));
        std::memcpy(&entry.length, &raw[slot_offset + 2], sizeof(entry.length));
        std::memcpy(&entry.is_active, &raw[slot_offset + 4], sizeof(entry.is_active));

        page.slot_directory.push_back(entry);
    }

    return page;
}


int Page::insertRecord(const std::vector<char> record) {
    int record_len = record.size();
    int space_needed = record_len + SLOT_ENTRY_SIZE;

    int slot_start = PAGE_HEADER_SIZE + (num_slots+1) * SLOT_ENTRY_SIZE;
    int record_end = free_space_offset;
    int record_start = record_end - record_len;

    // Check if record can be inserted or not
    // std::cout << slot_start << " " << record_start << " " << record_end << "\n"; 
    if (record_start < slot_start) { // allowing exact PAGE_SIZE fit.
        // throw std::out_of_range("record too large. Can't insert record in this page");
        return -1;
    }

    std::memcpy(&data[record_start], record.data(), record_len);


    SlotEntry entry;
    entry.offset = record_start;
    entry.length = record_len;
    entry.is_active = 1;

    // std::cout << "inserted record: " << record << " with slot id:" << num_slots << " "
    // << "and record start at" << record_start <<"\n";   

    slot_directory.push_back(entry);
    num_slots++;
    free_space_offset = record_start;

    return num_slots - 1;  // return slot ID
}

std::vector<char> Page::readRecord(int slot_id) const {
    if (slot_id < 0 || slot_id >= num_slots) {
        throw std::out_of_range("Invalid slot ID");
    }

    const SlotEntry& entry = slot_directory[slot_id];
    if (!entry.is_active) {
        throw std::runtime_error("Slot is deleted or inactive");
    }
    std::vector<char> record(entry.length);
    std::memcpy(record.data(),&data[entry.offset], entry.length);
    return record;
}


bool Page::deleteRecord(int slot_id) {
    if (slot_id < 0 || slot_id >= num_slots) {
        throw std::out_of_range("Invalid slot ID");
    }
    if(!slot_directory[slot_id].is_active){
        throw std::logic_error("Slot already deleted");
        return false;
    }
    slot_directory[slot_id].is_active = 0;
    return true;
}

uint32_t Page::getPageId() const{
    return page_id;
}
int Page::getFreeSpace() const{
    return free_space_offset - (PAGE_HEADER_SIZE+ num_slots*SLOT_ENTRY_SIZE);
}
int Page::getNumSlots() const{
    return num_slots;
}