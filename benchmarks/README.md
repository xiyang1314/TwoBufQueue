# Benchmark entry points

[English](README.md) · [简体中文](README.zh-CN.md)

[Performance evidence and reports](../docs/PERFORMANCE.md) · [Parameters and reproducible commands](TUNABLE_SPSC.md)

The active comparison target is `BRQ_BUILD_SPSC_BENCHMARKS=ON`: ordinary TwoBufQueue write and WriteSession against ReaderWriterQueue, rigtorp/SPSCQueue and atomic_queue with the same workload. Benchmark-only dependencies are explicitly downloaded, pinned and hash-checked by `bootstrap_spsc.py`; the library and default build do not depend on them.

`run_release_benchmarks.py` runs fixed configurations and saves complete reproduction evidence. `tune_spsc.py` searches parameters under the specified memory budget and platform conditions. `plot_release.py` plots saved formal results without running benchmarks.

For business execution after a message batch, use the [batch/business guide](BURST_CACHE.md). For the separate source-pool experiment, use the [hot-path cache guide](HOT_PATH_CACHE.md).

`BRQ_BUILD_BENCHMARKS` and `BRQ_BUILD_COMPARISON` are deprecated interfaces. Their historical implementations remain in the development directory and are not current measurement entry points.
