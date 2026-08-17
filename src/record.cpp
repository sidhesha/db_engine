#include "db_engine/record.hpp"
#include <stdexcept>
#include <cstring>
#include <iostream>

Record::Record(const std::vector<std::string>& fields)
    : fields_(fields) {}

Record::Record(const std::vector<std::string>& fields, uint64_t create_txn_id, RID prev_version)
    : fields_(fields), create_txn_id_(create_txn_id), prev_version_(prev_version) {}

std::vector<std::string> Record::getFields() const {
    return fields_;
}

std::vector<char> Record::serialize() const {

    // Layout: [create_txn_id][delete_txn_id][prev_page_id][prev_slot_id]
    // (the MVCC header, DELETE_TXN_ID_OFFSET/MVCC_HEADER_SIZE describe
    // it) followed by the original format: (int)number of fields +
    // [length][data][length][data]...

    std::vector<char> buffer;

    auto append = [&buffer](const void* src, size_t n) {
        size_t offset = buffer.size();
        buffer.resize(offset + n);
        std::memcpy(buffer.data() + offset, src, n);
    };

    append(&create_txn_id_, sizeof(create_txn_id_));
    append(&delete_txn_id_, sizeof(delete_txn_id_));
    append(&prev_version_.page_id, sizeof(prev_version_.page_id));
    append(&prev_version_.slot_id, sizeof(prev_version_.slot_id));

    int num_fields = static_cast<int>(fields_.size());
    append(&num_fields, sizeof(num_fields));

    for (const auto& field : fields_) {
        int len = static_cast<int>(field.size());
        append(&len, sizeof(len));
        append(field.data(), len);
    }

    return buffer;
}

Record Record::deserialize(const std::vector<char>& buffer) {
    if (buffer.size() < MVCC_HEADER_SIZE + sizeof(int)) {
        throw std::runtime_error("Invalid record buffer: too small");
    }

    size_t offset = 0;
    uint64_t create_txn_id, delete_txn_id;
    RID prev_version;

    std::memcpy(&create_txn_id, buffer.data() + offset, sizeof(create_txn_id));
    offset += sizeof(create_txn_id);
    std::memcpy(&delete_txn_id, buffer.data() + offset, sizeof(delete_txn_id));
    offset += sizeof(delete_txn_id);
    std::memcpy(&prev_version.page_id, buffer.data() + offset, sizeof(prev_version.page_id));
    offset += sizeof(prev_version.page_id);
    std::memcpy(&prev_version.slot_id, buffer.data() + offset, sizeof(prev_version.slot_id));
    offset += sizeof(prev_version.slot_id);

    int num_fields;
    std::memcpy(&num_fields, buffer.data() + offset, sizeof(int));
    offset += sizeof(int);

    std::vector<std::string> fields;
    for (int i = 0; i < num_fields; ++i) {
        if (offset + sizeof(int) > buffer.size()) {
            throw std::runtime_error("Invalid record buffer: length missing");
        }

        int len;
        std::memcpy(&len, buffer.data() + offset, sizeof(int));
        offset += sizeof(int);

        if (offset + len > buffer.size()) {
            throw std::runtime_error("Invalid record buffer: data missing");
        }

        std::string field(buffer.begin() + offset, buffer.begin() + offset + len);
        fields.push_back(field);
        offset += len;
    }

    Record rec(fields, create_txn_id, prev_version);
    rec.setDeleteTxnId(delete_txn_id);
    return rec;
}

size_t Record::size() const {
    size_t total = MVCC_HEADER_SIZE + sizeof(int); // MVCC header + number of fields
    for (const auto& field : fields_) {
        total += sizeof(int);
        total += field.size();
    }
    return total;
}
