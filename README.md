# B+ Tree Database Engine

A minimal database engine written in C++17 with B+ tree indexing, page-based storage, and catalog management.

## Quick Start

```powershell
cmake -S . -B build
cmake --build build
build\db_engine_test.exe
```

## Project Structure

```
db_engine/
├── include/           # Headers
├── src/               # Implementation
├── main.cpp           # Entry point
├── test.cpp           # All tests (single file)
├── CMakeLists.txt
└── AGENTS.md          # Build, test & workflow guide
```

## Workflow

Everything you need is in [`AGENTS.md`](./AGENTS.md):

| Step | Command |
|------|---------|
| Build | `cmake -S . -B build` then `cmake --build build` |
| Test | `build\db_engine_test.exe` |
| PR | Open a PR using the [template](.github/pull_request_template.md) |

**TDD cycle:** Write a failing test in `test.cpp` → implement → run all tests → commit.

## Features

- B+ Tree index (key-based search, insert, delete, range scan)
- Page-based disk storage with slot directories
- Record CRUD (insert, read, delete by key)
- Persistent catalog (table schemas saved to file)
- Modular design: index / storage / catalog / schema are decoupled