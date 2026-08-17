# Contributing / Agent Guide

## Prerequisites
- CMake 3.10+
- C++17 compiler (g++ via MSYS2 UCRT64 on Windows; any C++17 GCC/Clang elsewhere)

## Build

```powershell
cmake -S . -B build
cmake --build build
```

Produces three executables under `build/`:
- `db_engine` — the SQL server (`src/main.cpp`)
- `db_engine_test` — the correctness test suite (`tests/test.cpp`)
- `db_engine_bench` — the benchmark harness (`bench/bench.cpp`), always built at `-O2`
  regardless of `CMAKE_BUILD_TYPE`

## Test

```powershell
build\db_engine_test.exe
```

## Benchmark

```powershell
build\db_engine_bench.exe
```

Not a build gate — a slow or uninteresting number isn't a failure, but the harness must run
to completion without crashing. See `docs/BENCHMARKS.md` for captured numbers.

## About `tests/test.cpp`

All tests live in a single file. Each test function uses `<cassert>` and prints pass/fail;
`main()` runs every test function in sequence, grouped by section (`=== Section ===` banners).
No test framework dependency. When adding a feature:

1. Write the test function first (red)
2. Implement in `src/` and `include/db_engine/`
3. Wire the new function into `tests/test.cpp`'s `main()`
4. Build and run to confirm green

## Development Workflow (TDD)

1. Write a failing test
2. Implement until it passes
3. Build and run the **full** suite to confirm nothing else regressed
4. Commit

## Known build quirks

- **MinGW doesn't reliably handle brace-init through a single-argument constructor.** Always
  wrap in an explicit `std::vector<T>{...}`:
  ```cpp
  // GOOD
  Schema s(std::vector<Column>{{"id", "int"}});
  // BAD -- won't compile, or crashes at runtime, on MinGW
  Schema s({{"id", "int"}});
  ```
- **Windows headers reserve more identifiers than you'd expect.** `<winsock2.h>` (pulled by
  `db_engine/sqlserver.hpp`) drags in `<windows.h>`, whose `winnt.h` declares an unscoped enum
  with an enumerator literally named `TokenType` — colliding with an unrelated top-level type of
  the same name the moment that header enters the translation unit. `WIN32_LEAN_AND_MEAN` and
  `NOMINMAX` are defined before the `winsock2.h` include for exactly this reason; keep them there
  if this header is ever touched.

## PR Workflow

1. Branch from `master`
2. Write a test, implement, run the full suite (TDD, above)
3. Push, open a PR using `.github/pull_request_template.md`
4. Request review, merge after approval

**Hard rule: never merge on red.** Run the full local suite and confirm the CI check on the PR
is actually green before merging — no hacks or workarounds just to make a test "pass." This
project's history includes a run of PRs merged with CI silently broken for 3 merges straight
(see `docs/CONCURRENCY_BUGS.md` for the recovery); verify the check ran and passed, don't assume.
