#pragma once
#include <string>
#include <vector>

class Record {
public:
    Record() = default;
    Record(const std::vector<std::string>& fields);

    std::vector<std::string> getFields() const;

    std::vector<char> serialize() const;
    static Record deserialize(const std::vector<char>& buffer);

    size_t size() const;

private:
    std::vector<std::string> fields_;
};