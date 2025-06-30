# B+ Tree Database Engine (WIP)

Database engine written in C++17. Uses B+ trees for efficient indexing and storage. The project aims to provide a modular, extensible foundation for learning about database internals, including storage, indexing, and catalog management.

---

## Features

- **B+ Tree Index**: Fast key-based search, insert, update, and range scan.
- **Page-based Storage**: Fixed-size pages for efficient disk I/O.
- **Record Management**: Insert, read, and delete records with slot directory.
- **Catalog Management**: Persistent table schemas and metadata.
- **Schema Support**: Define tables with named columns and types.
- **Modular Design**: Clear separation of concerns (index, storage, catalog, schema).
- **CMake Build System**: Cross-platform, easy to extend.

---

## Project Structure

```
db_engine/
├── include/           # C++ header files
│   ├── bplustree.hpp      # B+ tree index
│   ├── node.hpp           # B+ tree node structure
│   ├── pagemanager.hpp    # Page file manager
│   ├── recordmanager.hpp  # Record CRUD logic
│   ├── catalogmanager.hpp # Table/catalog metadata
│   ├── schema.hpp         # Table schema definition
│   └── ...                # Other core headers
├── src/              # C++ source files
│   ├── bplustree.cpp
│   ├── node.cpp
│   ├── pagemanager.cpp
│   ├── recordmanager.cpp
│   ├── catalogmanager.cpp
│   ├── schema.cpp
│   └── ...                # Other implementations
├── main.cpp          # Example usage / entry point
├── test.cpp          # Basic test suite
├── CMakeLists.txt    # Build configuration
└── README.md         # Project documentation
```

---

## Build Instructions

**Requirements:**
- C++17 compatible compiler (e.g., g++ 7+, clang 6+, MSVC 2017+)
- CMake 3.10 or higher

**Steps:**
```bash
mkdir build
cd build
cmake ..
cmake --build .
```

---

## Usage

- **Run main application:**  
  `./db_engine`  
  (Demonstrates catalog and schema management.)

- **Run tests:**  
  `./db_engine_test`  
  (Basic tests for catalog and schema.)

---

## Current Status & Roadmap

> **This project is a work in progress.**  
> Many features are incomplete or experimental.  
> Contributions, bug reports, and suggestions are welcome!

**Planned/To-Do:**
- [ ] SQL-like query interface
- [ ] Transaction and concurrency support
- [ ] More robust error handling
- [ ] Performance optimizations
- [ ] Comprehensive test suite

---

## Core Concepts

- **B+ Tree**: Used for indexing records by key, supporting efficient range queries.
- **Page Manager**: Handles reading/writing fixed-size pages to disk.
- **Record Manager**: Manages record storage within pages, using slot directories.
- **Catalog Manager**: Stores and persists table schemas and metadata.
- **Schema**: Defines table columns and types.

---

## Contributing

This project is intended for learning and experimentation.  
Feel free to fork, open issues, or submit pull requests!

---