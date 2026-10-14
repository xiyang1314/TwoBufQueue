# Benchmark entry points

[English](README.md) · [简体中文](README.zh-CN.md)

Benchmarks are grouped by the workload they measure. The library's default build has no benchmark dependency. See [measurement scope](../docs/reports/PERFORMANCE.md) and the [parameter guide](guides/TUNABLE_SPSC.md).

## 1. Choose an experiment

| Directory | Purpose | Start here |
|---|---|---|
| `spsc/` | TwoBufQueue ordinary write / WriteSession versus pinned SPSC implementations | [Run comparisons](spsc/run_release_benchmarks.py) · [Search parameters](spsc/tune_spsc.py) |
| `burst/` | Business execution after a continuous message batch | [Run the experiment](burst/run_burst_empty.py) · [Guide](guides/BURST_CACHE.md) |
| `block_size/` | Writer-block size sweeps, with and without reader threads | [Full pipeline](block_size/test_per_writer_block_size.py) · [Writer only](block_size/test_per_write_block_size_only_writer.py) |
| `hot_path/` | Producer-memory and business-cache experiments | [Run the experiment](hot_path/test_hot_path_cache.py) · [Guide](guides/HOT_PATH_CACHE.md) |
| `tunable/` | Shared C++ workload, adapters and parameter handling | Used by the benchmark executables |
| `common/` | Shared process metrics | Used by the C++ experiments |
| `profiles/` | Saved workload and search configurations | Passed to the corresponding scripts |
| `guides/` | Parameter and experiment documentation | [SPSC guide](guides/TUNABLE_SPSC.md) |

## 2. Build the benchmark tools

From the repository root:

```sh
python3 benchmarks/spsc/bootstrap_spsc.py
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DBRQ_BUILD_SPSC_BENCHMARKS=ON
cmake --build build-bench --parallel
ctest --test-dir build-bench --output-on-failure -L performance_harness
```

Dependencies are explicitly downloaded, pinned and hash-checked. `run_release_benchmarks.py` saves complete reproduction evidence; `tune_spsc.py` searches parameters within the configured memory budget; [plot_release.py](spsc/plot_release.py) plots saved results without measuring again.

The block-depth experiments additionally require `BRQ_ENABLE_POOL_DEPTH_STATS=ON`. Formal performance comparisons use the default uninstrumented build. The executable names and command-line parameters are unchanged.

`BRQ_BUILD_BENCHMARKS` and `BRQ_BUILD_COMPARISON` are deprecated. Historical experiments remain in the original development directory.
