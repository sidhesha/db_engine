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

# std::thread needs this linked explicitly on some platforms/libc
# combinations (not all -- glibc on recent Ubuntu folds it into libc, but
# this stays portable rather than relying on that).
find_package(Threads REQUIRED)

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

# ws2_32 (Winsock2) only exists on Windows -- SqlServer's POSIX-socket
# branch (see include/db_engine/sqlserver.hpp) needs nothing extra on
# Linux/macOS beyond the pthread link above.
if(WIN32)
    set(PLATFORM_LIBS ws2_32)
else()
    set(PLATFORM_LIBS)
endif()

# Main executable
add_executable(db_engine src/main.cpp ${SOURCES})
target_link_libraries(db_engine Threads::Threads ${PLATFORM_LIBS})

# Test executable
add_executable(db_engine_test tests/test.cpp ${SOURCES})
target_link_libraries(db_engine_test Threads::Threads ${PLATFORM_LIBS})

# Benchmark executable. Always built optimized regardless of the active
# CMAKE_BUILD_TYPE -- a benchmark run against the -O0 debug build the
# test suite uses would produce meaningless numbers.
add_executable(db_engine_bench bench/bench.cpp ${SOURCES})
target_link_libraries(db_engine_bench Threads::Threads ${PLATFORM_LIBS})
target_compile_options(db_engine_bench PRIVATE -O2)