# Compiler is resolved from PATH by CMake's own toolchain detection
# (project() below) rather than a hardcoded absolute path, so this
# configures identically on any machine, including CI, as long as a
# MinGW/UCRT64 gcc/g++ is on PATH.
cmake_minimum_required(VERSION 3.10)
project(db_engine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add debug flags for UCRT64
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g3 -O0")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g3 -O0")

# Include directories
include_directories(include)

# Source files
set(SOURCES
    src/bplustree.cpp
    src/indexmanager.cpp
    src/node.cpp
    src/pagemanager.cpp
    src/record.cpp
    src/page.cpp
    src/recordmanager.cpp
    src/schema.cpp
    src/catalogmanager.cpp
    src/bufferpool.cpp
    src/table.cpp
    src/wal.cpp
    src/recoverymanager.cpp
    src/mvcc.cpp
    src/lockmanager.cpp
    src/database.cpp
    src/lexer.cpp
    src/parser.cpp
    src/executor.cpp
    src/sqlserver.cpp
    src/evictionpolicy.cpp
)

# Main executable
add_executable(db_engine main.cpp ${SOURCES})
target_link_libraries(db_engine ws2_32)

# Test executable
add_executable(db_engine_test test.cpp ${SOURCES})
target_link_libraries(db_engine_test ws2_32)