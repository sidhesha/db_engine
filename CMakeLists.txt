cmake_minimum_required(VERSION 3.10)
project(db_engine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set UCRT64 toolchain
set(CMAKE_C_COMPILER C:/msys64/ucrt64/bin/gcc.exe)
set(CMAKE_CXX_COMPILER C:/msys64/ucrt64/bin/g++.exe)

# Add debug flags for UCRT64
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g3 -O0")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g3 -O0")

# Include directories
include_directories(include)

# Source files
set(SOURCES
    src/bplustree.cpp
    src/node.cpp
    src/pagemanager.cpp
    src/record.cpp
    src/page.cpp
)

# Main executable
add_executable(db_engine main.cpp ${SOURCES})

# Test executable
add_executable(db_engine_test test.cpp ${SOURCES})