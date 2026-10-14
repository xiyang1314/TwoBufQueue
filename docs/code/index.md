[Skip to source](#source-browser)

[TwoBufQueue C++](https://xiyang1314.github.io/TwoBufQueue/README.html)

[Overview](https://xiyang1314.github.io/TwoBufQueue/README.html) · [API](https://xiyang1314.github.io/TwoBufQueue/docs/guides/API.html) · [Examples](https://xiyang1314.github.io/TwoBufQueue/examples/README.html) · [Source browser](https://xiyang1314.github.io/TwoBufQueue/docs/code/index.html)

[English](https://xiyang1314.github.io/TwoBufQueue/docs/code/index.html) · [中文](https://xiyang1314.github.io/TwoBufQueue/docs/code/index.zh-CN.html)

TWOBUFQUEUE · SOURCE BROWSER

# Source browser

Start with basic messaging, then choose grouped, concurrent or synchronous callbacks. Every view contains the complete source, syntax highlighting and line numbers.

[Start with the basic example](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/01_basic.cpp.html) · [API guide](https://xiyang1314.github.io/TwoBufQueue/docs/guides/API.html)

[Examples](#examples) · [Components](#components) · [Benchmark scripts](#benchmark-scripts) · [Assembly](#assembly) · [Build integration](#build-integration) · [Technical notes](#technical-notes)

## Examples

Basic messaging, grouped callbacks, concurrency and synchronous callbacks.

[01_basic.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/01_basic.cpp.html)

Create a Writer, receive messages, finish production and drain.

examples/01_basic.cpp

C++ · 42 lines

[02_batch_callback.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/02_batch_callback.cpp.html)

Receive grouped data and iterate the records in each source range.

examples/two_buf_queue/02_batch_callback.cpp

C++ · 34 lines

[03_multi_thread_writer.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/03_multi_thread_writer.cpp.html)

One Writer per producer, with background consumption and validation.

examples/two_buf_queue/03_multi_thread_writer.cpp

C++ · 63 lines

[04_layer1_callback.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/04_layer1_callback.cpp.html)

Read layer-1 source blocks directly, without layer-2 copying.

examples/two_buf_queue/04_layer1_callback.cpp

C++ · 62 lines

[05_seq_writer.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/05_seq_writer.cpp.html)

Assign sequence numbers and receive source, sequence and payload.

examples/two_buf_queue/05_seq_writer.cpp

C++ · 74 lines

[06_sync_writer.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/06_sync_writer.cpp.html)

Run the callback in the producer thread before write returns.

examples/two_buf_queue/06_sync_writer.cpp

C++ · 49 lines

[07_quick_write_muti_msg.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/07_quick_write_muti_msg.cpp.html)

Nested loops write the current block and explicitly acquire the next.

examples/two_buf_queue/07_quick_write_muti_msg.cpp

C++ · 48 lines

[08_all_parameters.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/08_all_parameters.cpp.html)

All Writer, pool, reader and consumer settings.

examples/two_buf_queue/08_all_parameters.cpp

C++ · 111 lines

[09_async_logger.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/09_async_logger.cpp.html)

Background file logging, bounded-buffer pressure and draining shutdown.

examples/two_buf_queue/09_async_logger.cpp

C++ · 114 lines

[11_warm_pool.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/components/11_warm_pool.cpp.html)

Borrow and return pooled resources, and observe reuse.

examples/components/11_warm_pool.cpp

C++ · 48 lines

[12_warm_pool_factory.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/components/12_warm_pool_factory.cpp.html)

Customize resource creation and destruction.

examples/components/12_warm_pool_factory.cpp

C++ · 55 lines

[13_spsc_live_handoff_queue.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/components/13_spsc_live_handoff_queue.cpp.html)

Read an active resource range and return it after processing.

examples/components/13_spsc_live_handoff_queue.cpp

C++ · 149 lines

[14_spsc_live_handoff_queue_claim.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/components/14_spsc_live_handoff_queue_claim.cpp.html)

Claim the head; cancel to retain work that cannot yet be processed.

examples/components/14_spsc_live_handoff_queue_claim.cpp

C++ · 155 lines

[15_handoff_queue.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/components/15_handoff_queue.cpp.html)

Built-in consumers process handoffs and retry rejected work.

examples/components/15_handoff_queue.cpp

C++ · 96 lines

[16_handoff_queue_mpmc.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/components/16_handoff_queue_mpmc.cpp.html)

Concurrent producers and consumers share a handoff queue.

examples/components/16_handoff_queue_mpmc.cpp

C++ · 87 lines

[example_support.hpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/example_support.hpp.html)

Shared example checks, parsing and helpers.

examples/example_support.hpp

C++ · 54 lines

## Components

Read these headers for lower-level behavior.

[handoff_queue.hpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/src/base_components/HandoffQueue/handoff_queue.hpp.html)

Resource handoff with multiple concurrency modes and consumer workers.

src/base_components/HandoffQueue/handoff_queue.hpp

C++ · 102 lines

[spsc_live_handoff_queue.hpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/src/base_components/LiveHandoffQueue/spsc_live_handoff_queue.hpp.html)

SPSC resource handoff with access to data still being produced.

src/base_components/LiveHandoffQueue/spsc_live_handoff_queue.hpp

C++ · 353 lines

[warm_pool.hpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/src/base_components/WarmPool/warm_pool.hpp.html)

Resource creation, borrowing, return and capacity control.

src/base_components/WarmPool/warm_pool.hpp

C++ · 375 lines

## Benchmark scripts

Execution, parameter selection and saved results.

[test_per_write_block_size_only_writer.py](https://xiyang1314.github.io/TwoBufQueue/docs/code/benchmarks/block_size/test_per_write_block_size_only_writer.py.html)

Complete source referenced by the documentation.

benchmarks/block_size/test_per_write_block_size_only_writer.py

Python · 254 lines

[test_per_writer_block_size.py](https://xiyang1314.github.io/TwoBufQueue/docs/code/benchmarks/block_size/test_per_writer_block_size.py.html)

Complete source referenced by the documentation.

benchmarks/block_size/test_per_writer_block_size.py

Python · 324 lines

[run_burst_empty.py](https://xiyang1314.github.io/TwoBufQueue/docs/code/benchmarks/burst/run_burst_empty.py.html)

Complete source referenced by the documentation.

benchmarks/burst/run_burst_empty.py

Python · 158 lines

[test_hot_path_cache.py](https://xiyang1314.github.io/TwoBufQueue/docs/code/benchmarks/hot_path/test_hot_path_cache.py.html)

Complete source referenced by the documentation.

benchmarks/hot_path/test_hot_path_cache.py

Python · 285 lines

[bootstrap_spsc.py](https://xiyang1314.github.io/TwoBufQueue/docs/code/benchmarks/spsc/bootstrap_spsc.py.html)

Complete source referenced by the documentation.

benchmarks/spsc/bootstrap_spsc.py

Python · 55 lines

[plot_release.py](https://xiyang1314.github.io/TwoBufQueue/docs/code/benchmarks/spsc/plot_release.py.html)

Plot saved results without rerunning benchmarks.

benchmarks/spsc/plot_release.py

Python · 70 lines

[run_release_benchmarks.py](https://xiyang1314.github.io/TwoBufQueue/docs/code/benchmarks/spsc/run_release_benchmarks.py.html)

Run configured measurements, validate payloads and save all records.

benchmarks/spsc/run_release_benchmarks.py

Python · 220 lines

[tune_spsc.py](https://xiyang1314.github.io/TwoBufQueue/docs/code/benchmarks/spsc/tune_spsc.py.html)

Complete source referenced by the documentation.

benchmarks/spsc/tune_spsc.py

Python · 738 lines

[run.py](https://xiyang1314.github.io/TwoBufQueue/docs/code/docs/performance_data/latency32_investigation_20261010/run.py.html)

Historical experiment script, preserving its measured logic.

docs/performance_data/latency32_investigation_20261010/run.py

Python · 43 lines

[restyle_comparison.py](https://xiyang1314.github.io/TwoBufQueue/docs/code/docs/performance_data/plateau_compare_256m_zero_20261012/restyle_comparison.py.html)

Historical experiment script, preserving its measured logic.

docs/performance_data/plateau_compare_256m_zero_20261012/restyle_comparison.py

Python · 312 lines

[select_weighted.py](https://xiyang1314.github.io/TwoBufQueue/docs/code/docs/performance_data/plateau_compare_256m_zero_20261012/weighted_common_config_20261012/select_weighted.py.html)

Historical experiment script, preserving its measured logic.

docs/performance_data/plateau_compare_256m_zero_20261012/weighted_common_config_20261012/select_weighted.py

Python · 185 lines

## Assembly

Actual instructions saved during performance diagnosis.

[business_original.asm](https://xiyang1314.github.io/TwoBufQueue/docs/code/docs/performance_data/burst_instruction_1m_20261012/business_original.asm.html)

Saved assembly with line navigation.

docs/performance_data/burst_instruction_1m_20261012/business_original.asm

Assembly · 45 lines

[push_codegen.s](https://xiyang1314.github.io/TwoBufQueue/docs/code/docs/performance_data/rigtorp_prefetch_20261011/push_codegen.s.html)

Saved assembly with line navigation.

docs/performance_data/rigtorp_prefetch_20261011/push_codegen.s

Assembly · 66 lines

## Build integration

CMake configuration for application integration.

[CMakeLists.txt](https://xiyang1314.github.io/TwoBufQueue/docs/code/test/integration/CMakeLists.txt.html)

Check vendored, FetchContent and installed-package integration.

test/integration/CMakeLists.txt

CMake · 32 lines

## Technical notes

Saved diagnostic notes and experiments.

[README.txt](https://xiyang1314.github.io/TwoBufQueue/docs/code/docs/performance_data/twobufqueue_8b_regression_20261011/README.txt.html)

Saved technical notes and experiments.

docs/performance_data/twobufqueue_8b_regression_20261011/README.txt

Text · 35 lines

TwoBufQueue · C++ · Apache-2.0 [Overview](https://xiyang1314.github.io/TwoBufQueue/README.html)
