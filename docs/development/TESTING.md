# Testing and validation

[English](https://xiyang1314.github.io/TwoBufQueue/docs/development/TESTING.html) · [简体中文](https://xiyang1314.github.io/TwoBufQueue/docs/development/TESTING.zh-CN.html)

The current standalone Release build includes library regression tests, functional suites, runnable examples and integration checks. A fresh copy containing only publication-manifest files passed **567 / 567 tests** on the local macOS host during English-documentation preparation. This records correctness checks, not a new performance measurement or Linux/Windows result.

## 1. Run the current suite

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

| Area | What is checked |
|---|---|
| Writer and WriteSession | Complete bytes, boundaries, publication, pressure, explicit block switching and lifetimes |
| Layer-1 readers | Completed-block priority, current snapshots, targeted notifications, retries, cancellation, shutdown and single/multiple Writer implementations |
| Block and pool components | Ownership, alignment, slices, generations, resource return, limits and reclamation |
| Handoff queues | SPSC/MPMC transfer, claims, cancellation modes and concurrent data integrity |
| Complete pipeline | Copying, per-source FIFO, exactly-once handling and resource return |
| Examples | Runnable APIs, synchronous callbacks, grouped callbacks and asynchronous file logging |
| Build/integration | Vendored source, local FetchContent and a relocated installed package |

Each case is registered independently with CTest. A passing subset or one matrix parameter does not prove the entire suite passed. Timeouts protect against deadlocks; they are not performance/tail-latency evidence. Concurrency checks use controlled synchronization and verify all sequences rather than relying on sleeps to infer ordering.

## 2. Inspect one part

```sh
ctest --test-dir build -C Release -L read_ctrl --output-on-failure
ctest --test-dir build -C Release -R '^read_ctrl_notification_' --output-on-failure
ctest --test-dir build -C Release -R '^read_ctrl_single_' --output-on-failure
ctest --test-dir build -C Release -L catalog --output-on-failure
```

Machine-readable case catalogs and their Markdown tables are kept identical; their historical scenario text currently retains the original Chinese. Case identifiers and runnable test code are shared, with English source comments and diagnostics. These fixtures are not a separate Chinese implementation.

## 3. Historical evidence

The [dated reader checks (Chinese)](https://xiyang1314.github.io/TwoBufQueue/test/reader/READ_CTRL_TESTS.html) distinguish the test coverage available on each date. The [saved release checks](../release_checks/summary.json) and [earlier validation records](../performance_data/release_20261011/release_checks/summary.json) retain their recorded platforms, commands and limits. Do not treat an old subset count as today's full-suite count or claim a platform that did not run.

CI is configured for Linux, Windows and macOS, plus sanitizers and optional pinned SPSC adapters. Remote CI execution is still pending until the repository is published and the workflows run. Source/UI translation does not change the library's non-comment C++ content or the historical measured-source archives.
