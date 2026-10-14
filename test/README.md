# Tests

[English](https://xiyang1314.github.io/TwoBufQueue/test/README.html) · [简体中文](https://xiyang1314.github.io/TwoBufQueue/test/README.zh-CN.html)

Choose a directory by the behavior being tested. Source locations are grouped; CMake targets and CTest case names keep their existing names.

## 1. Directory guide

| Directory | Coverage |
|---|---|
| `writer/` | Writer, write sessions, synchronous callbacks and writer queues |
| `reader/` | Current-block scanning, notifications, single/multiple-writer reading and completion; [dated checks](https://xiyang1314.github.io/TwoBufQueue/test/reader/READ_CTRL_TESTS.html) |
| `queue/` | Full-queue options and unified message delivery |
| `pool/` | Slice allocation, capacity and release checks |
| `handoff/` | Live resource handoff, current snapshots and concurrent FIFO |
| `performance/` | Benchmark harness contracts and Python search/report tests; these check correctness rather than publish performance scores |
| `support/` | Shared test fixtures and helper headers |
| `func/` | Detailed functional and concurrent contracts; [case catalog](https://xiyang1314.github.io/TwoBufQueue/test/func/TEST_CASES.html) |
| `integration/` | Vendored, FetchContent and installed-package integration |

## 2. Run tests

From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Benchmark harness tests are enabled with `BRQ_BUILD_SPSC_BENCHMARKS=ON` after [bootstrapping pinned dependencies](https://xiyang1314.github.io/TwoBufQueue/docs/code/benchmarks/spsc/bootstrap_spsc.py.html). See [the test guide](https://xiyang1314.github.io/TwoBufQueue/docs/development/TESTING.html) and [benchmark navigation](https://xiyang1314.github.io/TwoBufQueue/benchmarks/README.html).
