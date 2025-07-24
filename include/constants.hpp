#pragma once

constexpr int ORDER = 4;
constexpr int MIN_KEYS = (ORDER - 1) / 2;
constexpr int MAX_KEYS = ORDER - 1;
constexpr int MIN_CHILDREN = (ORDER + 1) / 2;
constexpr int MAX_CHILDREN = ORDER;

struct RID {
    int page_id;
    int slot_id;
};