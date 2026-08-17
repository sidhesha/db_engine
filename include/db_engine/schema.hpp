#pragma once

#include <string>
#include <vector>

struct Column {
    std::string name;
    std::string type; // "int", "string", ...

    Column() = default;
    Column(const std::string& n, const std::string& t) : name(n), type(t) {}
};

class Schema {
public:
    Schema() = default;
    Schema(const std::vector<Column>& columns);

    const std::vector<Column>& getColumns()const;

    std::string serialize() const;
    static Schema deserialize(const std::string& data);

private:
    std::vector<Column> columns;
};
