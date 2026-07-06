# Build & Test Commands

## Prerequisites
- CMake 3.10+
- C++17 compiler (g++ via MSYS2 UCRT64)

## Build
```powershell
cmake -S . -B build
cmake --build build
```

## Test
```powershell
build\db_engine_test.exe
```

## About `test.cpp`
All tests live in a single `test.cpp` file. Each test function is a simple function that uses `<cassert>` and prints pass/fail. The `main()` in `test.cpp` runs all test functions in sequence. When adding a new feature:
1. Write the test function in `test.cpp` first (red)
2. Implement the feature in `src/` and `include/`
3. Wire up the test function in `test.cpp::main()`
4. Build & run tests to confirm green

## Development Workflow (TDD)
1. Write a failing test in `test.cpp`
2. Implement until test passes
3. Build & run all tests to confirm nothing regressed
4. Commit

## PR Workflow
1. Create a feature branch from `main`
2. Make changes with TDD (above)
3. Push branch and open PR using the PR template
4. Request review
5. After approval, merge via PR
