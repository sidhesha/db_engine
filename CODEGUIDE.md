# Codebase Guide

## Build & Test
```powershell
cmake -S . -B build
cmake --build build
build\db_engine_test.exe
```

## Architecture

### Components (include/*.hpp, src/*.cpp)

| Component | Header | Role |
|-----------|--------|------|
| **Key** | `key.hpp` | Variant type (int/float/string) with comparison operators. No source file. |
| **Schema** | `schema.hpp`, `schema.cpp` | Column definitions (name+type). Serialize/deserialize to string. |
| **Record** | `record.hpp`, `record.cpp` | Row data (vector<string>). Serialize/deserialize with length-prefixed fields. |
| **Page** | `page.hpp`, `page.cpp` | Fixed 4096-byte page with slot directory. Insert/read/delete records. |
| **PageManager** | `pagemanager.hpp`, `pagemanager.cpp` | File I/O for pages. Allocate, read, write by page_id. Opens file on construction. |
| **RecordManager** | `recordmanager.hpp`, `recordmanager.cpp` | Insert/read/delete Record objects using PageManager. Auto-allocates pages. |
| **BPlusTreeNode** | `node.hpp`, `node.cpp` | Node with keys + RIDs (leaf) or keys + children (internal). Split, merge, serialize. |
| **BPlusTree** | `bplustree.hpp`, `bplustree.cpp` | B+ tree index with optional disk persistence. Insert, search, update, rangeScan, remove, save, load. |
| **CatalogManager** | `catalogmanager.hpp`, `catalogmanager.cpp` | Table schema catalog. Saves/loads from text file. |
| **Table** | `table.hpp`, `table.cpp` | High-level API combining RecordManager + BPlusTree. Insert/getByKey/deleteByKey. |
| **IndexManager** | `indexmanager.hpp`, `indexmanager.cpp` | Persists B+ tree nodes to disk. Allocates node IDs, saves/loads individual nodes, stores root_node_id header. |
| **constants.hpp** | — | ORDER, PAGE_SIZE, RID struct. |

## Key Patterns

### Constructor calls (MinGW quirk)
Always use explicit vector type to avoid ambiguity:
```cpp
// GOOD
Schema s(std::vector<Column>{{"id", "int"}});
Record r(std::vector<std::string>{"a", "b"});

// BAD - won't compile or crashes at runtime on MinGW
Schema s({{"id", "int"}});
Record r({"a", "b"});
```

### File cleanup in tests
Each test function that creates files must:
1. `std::remove(file.c_str())` before and after
2. Use unique filenames per function (e.g., `test_rm.db`, `test_cat.txt`)

### Test format
```cpp
TEST("description") {
    // arrange, act, assert
} END_TEST;
```
Uses `passed`/`failed` globals with try/catch. No test framework dependency.

### B+ Tree Constants (constants.hpp)
- `ORDER = 4` → max 3 keys per node, min 1 (except root)
- `PAGE_SIZE = 4096`

## Gotchas
- **MinGW doesn't handle brace-init with single-argument constructors** — always wrap in explicit `std::vector<T>{...}`
- `isFull()` checks `keys.size() > ORDER-1` (note: `>` not `>=`)
- `PageManager` computes `next_page_id` from file size on open
- `BPlusTree::remove()` has underflow handling but no rebalancing for root-as-leaf edge case fully tested
- `RecordManager::insertRecord()` scans all existing pages linearly before allocating new
- `Table::deleteByKey()` calls `BPlusTree::remove()` which is the `remove` method
- `IndexManager` stores a 4-byte `root_node_id` header at offset 0; nodes start at offset `PAGE_SIZE`
- `BPlusTree(IndexManager&)` auto-loads; `save()` persists the entire tree; `load()` reconstructs `next_leaf` chain
- `IndexManager::loadNode()` returns stubs for internal-node children — `BPlusTree::load()` recursively loads them

## PR Workflow
1. Branch from `main`
2. Write test in `test.cpp`, implement, run all tests
3. Push, open PR using `.github/pull_request_template.md`
4. Request review, merge after approval
