#pragma once

#include <cstddef>
#include <functional>

constexpr int ORDER = 4;
constexpr int MIN_KEYS = (ORDER - 1) / 2;
constexpr int MAX_KEYS = ORDER - 1;
constexpr int MIN_CHILDREN = (ORDER + 1) / 2;
constexpr int MAX_CHILDREN = ORDER;
constexpr int PAGE_SIZE = 4096;

struct RID {
    int page_id;
    int slot_id;
};

inline bool operator==(const RID& a, const RID& b) {
    return a.page_id == b.page_id && a.slot_id == b.slot_id;
}

inline bool operator!=(const RID& a, const RID& b) {
    return !(a == b);
}

// Lets RID key a std::unordered_map/std::unordered_set -- needed by the
// MVCC lock table (Phase 5), which is keyed per logical row.
namespace std {
template <>
struct hash<RID> {
    std::size_t operator()(const RID& rid) const noexcept {
        std::size_t h1 = std::hash<int>{}(rid.page_id);
        std::size_t h2 = std::hash<int>{}(rid.slot_id);
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};
}  // namespace std