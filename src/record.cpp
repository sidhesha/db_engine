#include "record.hpp"
#include <stdexcept>
#include <cstring>
#include <iostream>

Record::Record(const std::vector<std::string>& fields)
    : fields_(fields) {}

std::vector<std::string> Record::getFields() const {
    return fields_;
}

std::vector<char> Record::serialize() const {

    // steps: (int)number of fields + [length][data][length][data][length][data]... 

    std::vector<char> buffer;

    int num_fields = static_cast<int>(fields_.size());
    buffer.resize(sizeof(int));
    std::memcpy(buffer.data(), &num_fields, sizeof(int));

    // 2. Append each field as [length][data]
    for (const auto& field : fields_) {
        int len = static_cast<int>(field.size());
        size_t offset = buffer.size();

        buffer.resize(offset + sizeof(int) + len);
        std::memcpy(buffer.data() + offset, &len, sizeof(int));
        std::memcpy(buffer.data() + offset + sizeof(int), field.data(), len);
    }

    return buffer;
}

Record Record::deserialize(const std::vector<char>& buffer) {
    std::vector<std::string> fields;

    size_t offset = 0;
    if (buffer.size() < sizeof(int)) {
        throw std::runtime_error("Invalid record buffer: too small");
    }

    int num_fields;
    std::memcpy(&num_fields, buffer.data(), sizeof(int));
    offset += sizeof(int);

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

    return Record(fields);
}

size_t Record::size() const {
    size_t total = sizeof(int); // number of fields
    for (const auto& field : fields_) {
        total += sizeof(int);
        total += field.size();          
    }
    return total;
}