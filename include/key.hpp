#pragma once
#include <variant>
#include <string>
#include <stdexcept>

enum class KeyTypeTag {
    INTEGER,
    FLOAT,
    STRING
};



class Key {
public:
    std::variant<int, float, std::string> value;

    Key() = default;

    Key(int v) : value(v) {}
    Key(float v) : value(v) {}
    Key(const std::string& v) : value(v) {}

    KeyTypeTag getType() const {
        if (std::holds_alternative<int>(value)) return KeyTypeTag::INTEGER;
        if (std::holds_alternative<float>(value)) return KeyTypeTag::FLOAT;
        return KeyTypeTag::STRING;
    }

    bool operator>(const Key& other) const {
        return compare(other) > 0;
    }
    
    bool operator<(const Key& other) const {
        return compare(other) < 0;
    }

    bool operator==(const Key& other) const {
        return compare(other) == 0;
    }

    bool operator!=(const Key& other) const {
        return !(*this == other);
    }

    bool operator<=(const Key& other) const {
        return compare(other) <= 0;
    }

    bool operator>=(const Key& other) const {
        return compare(other) >= 0;
    }
    

    std::string toString() const {
        if (std::holds_alternative<int>(value)) return std::to_string(std::get<int>(value));
        if (std::holds_alternative<float>(value)) return std::to_string(std::get<float>(value));
        return std::get<std::string>(value);
    }

private:
    int compare(const Key& other) const {
        if (value.index() != other.value.index()) {
            throw std::runtime_error("Type mismatch in key comparison");
        }

        if (std::holds_alternative<int>(value)) {
            int a = std::get<int>(value);
            int b = std::get<int>(other.value);
            return (a > b) - (a < b);
        }

        if (std::holds_alternative<float>(value)) {
            float diff = std::get<float>(value) - std::get<float>(other.value);
            return (diff > 0) - (diff < 0);
        }

        return std::get<std::string>(value).compare(std::get<std::string>(other.value));
    }
};
