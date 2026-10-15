# Benchmark entry points

[English](https://xiyang1314.github.io/TwoBufQueue/benchmarks/README.html) · [简体中文](https://xiyang1314.github.io/TwoBufQueue/benchmarks/README.zh-CN.html)

Benchmarks are grouped by the workload they measure. The library's default build has no benchmark dependency. See [measurement scope](https://xiyang1314.github.io/TwoBufQueue/docs/reports/PERFORMANCE.html) and the [parameter guide](https://xiyang1314.github.io/TwoBufQueue/benchmarks/guides/TUNABLE_SPSC.html).

## 1. Choose an experiment

| Directory | Purpose | Start here |
|---|---|---|
| `spsc/` | TwoBufQueue ordinary write / WriteSession versus pinned SPSC implementations | [Run comparisons](https://xiyang1314.github.io/TwoBufQueue/docs/code/benchmarks/spsc/run_release_benchmarks.py.html) · [Search parameters](https://xiyang1314.github.io/TwoBufQueue/docs/code/benchmarks/spsc/tune_spsc.py.html) |
| `burst/` | Business execution after a continuous message batch | [Run the experiment](https://xiyang1314.github.io/TwoBufQueue/docs/code/benchmarks/burst/run_burst_empty.py.html) · [Guide](https://xiyang1314.github.io/TwoBufQueue/benchmarks/guides/BURST_CACHE.html) |
| `block_size/` | Writer-block size sweeps, with and without reader threads | [Full pipeline](https://xiyang1314.github.io/TwoBufQueue/docs/code/benchmarks/block_size/test_per_writer_block_size.py.html) · [Writer only](https://xiyang1314.github.io/TwoBufQueue/docs/code/benchmarks/block_size/test_per_write_block_size_only_writer.py.html) |
| `hot_path/` | Producer-memory and business-cache experiments | [Run the experiment](https://xiyang1314.github.io/TwoBufQueue/docs/code/benchmarks/hot_path/test_hot_path_cache.py.html) · [Guide](https://xiyang1314.github.io/TwoBufQueue/benchmarks/guides/HOT_PATH_CACHE.html) |
| `tunable/` | Shared C++ workload, adapters and parameter handling | Used by the benchmark executables |
| `common/` | Shared process metrics | Used by the C++ experiments |
| `profiles/` | Saved workload and search configurations | Passed to the corresponding scripts |
| `guides/` | Parameter and experiment documentation | [SPSC guide](https://xiyang1314.github.io/TwoBufQueue/benchmarks/guides/TUNABLE_SPSC.html) |

## 2. Build the benchmark tools

From the repository root:

```sh
python3 benchmarks/spsc/bootstrap_spsc.py --check-only
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DBRQ_BUILD_SPSC_BENCHMARKS=ON
cmake --build build-bench --parallel
ctest --test-dir build-bench --output-on-failure -L performance_harness
```

Pinned headers and licenses are included in `third_party/spsc/`; the command above verifies them without network access. See [bundled dependency details](https://xiyang1314.github.io/TwoBufQueue/third_party/spsc/README.html). `run_release_benchmarks.py` saves complete reproduction evidence; `tune_spsc.py` searches parameters within the configured memory budget; [plot_release.py](https://xiyang1314.github.io/TwoBufQueue/docs/code/benchmarks/spsc/plot_release.py.html) plots saved results without measuring again.

The block-depth experiments additionally require `BRQ_ENABLE_POOL_DEPTH_STATS=ON`. Formal performance comparisons use the default uninstrumented build. The executable names and command-line parameters are unchanged.

`BRQ_BUILD_BENCHMARKS` and `BRQ_BUILD_COMPARISON` are deprecated. Historical experiments remain in the original development directory.
