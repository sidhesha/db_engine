# B+ Tree Database Engine

A simple database engine implementation using B+ trees for efficient data storage and retrieval.

## Project Structure

```
db_engine/
├── include/           # Header files
│   ├── bplustree.hpp  # B+ tree implementation
│   ├── node.hpp       # Node structure definitions
│   ├── pagemanager.hpp# Page management system
├── src/              # Source files
│   ├── bplustree.cpp
│   ├── node.cpp
│   ├── pagemanager.cpp
├── main.cpp          # Main application
├── test.cpp          # Test suite
└── CMakeLists.txt    # Build configuration
```

## Building the Project

```bash
mkdir build
cd build
cmake ..
cmake --build .
```

## Running

- Main application: `./db_engine`
- Tests: `./db_engine_test`

## Requirements

- C++17 compatible compiler
- CMake 3.10 or higher 