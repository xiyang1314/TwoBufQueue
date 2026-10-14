[Skip to source](#source-browser)

[TwoBufQueue C++](../../README.md)

[Overview](../../README.md) · [API](../guides/API.md) · [Examples](../../examples/README.md) · [Source browser](index.md)

[English](index.md) · [中文](index.zh-CN.md)

TWOBUFQUEUE · SOURCE BROWSER

# Source browser

Start with basic messaging, then choose grouped, concurrent or synchronous callbacks. Every view contains the complete source, syntax highlighting and line numbers.

[Start with the basic example](../../examples/01_basic.cpp) · [API guide](../guides/API.md)

[Examples](#examples) · [Components](#components) · [Benchmark scripts](#benchmark-scripts) · [Assembly](#assembly) · [Build integration](#build-integration) · [Technical notes](#technical-notes)

## Examples

Basic messaging, grouped callbacks, concurrency and synchronous callbacks.

[01_basic.cpp](../../examples/01_basic.cpp)

Create a Writer, receive messages, finish production and drain.

examples/01_basic.cpp

C++ · 42 lines

[02_batch_callback.cpp](../../examples/two_buf_queue/02_batch_callback.cpp)

Receive grouped data and iterate the records in each source range.

examples/two_buf_queue/02_batch_callback.cpp

C++ · 34 lines

[03_multi_thread_writer.cpp](../../examples/two_buf_queue/03_multi_thread_writer.cpp)

One Writer per producer, with background consumption and validation.

examples/two_buf_queue/03_multi_thread_writer.cpp

C++ · 63 lines

[04_layer1_callback.cpp](../../examples/two_buf_queue/04_layer1_callback.cpp)

Read layer-1 source blocks directly, without layer-2 copying.

examples/two_buf_queue/04_layer1_callback.cpp

C++ · 62 lines

[05_seq_writer.cpp](../../examples/two_buf_queue/05_seq_writer.cpp)

Assign sequence numbers and receive source, sequence and payload.

examples/two_buf_queue/05_seq_writer.cpp

C++ · 74 lines

[06_sync_writer.cpp](../../examples/two_buf_queue/06_sync_writer.cpp)

Run the callback in the producer thread before write returns.

examples/two_buf_queue/06_sync_writer.cpp

C++ · 49 lines

[07_quick_write_muti_msg.cpp](../../examples/two_buf_queue/07_quick_write_muti_msg.cpp)

Nested loops write the current block and explicitly acquire the next.

examples/two_buf_queue/07_quick_write_muti_msg.cpp

C++ · 48 lines

[08_all_parameters.cpp](../../examples/two_buf_queue/08_all_parameters.cpp)

All Writer, pool, reader and consumer settings.

examples/two_buf_queue/08_all_parameters.cpp

C++ · 111 lines

[09_async_logger.cpp](../../examples/two_buf_queue/09_async_logger.cpp)

Background file logging, bounded-buffer pressure and draining shutdown.

examples/two_buf_queue/09_async_logger.cpp

C++ · 114 lines

[11_warm_pool.cpp](../../examples/components/11_warm_pool.cpp)

Borrow and return pooled resources, and observe reuse.

examples/components/11_warm_pool.cpp

C++ · 48 lines

[12_warm_pool_factory.cpp](../../examples/components/12_warm_pool_factory.cpp)

Customize resource creation and destruction.

examples/components/12_warm_pool_factory.cpp

C++ · 55 lines

[13_spsc_live_handoff_queue.cpp](../../examples/components/13_spsc_live_handoff_queue.cpp)

Read an active resource range and return it after processing.

examples/components/13_spsc_live_handoff_queue.cpp

C++ · 149 lines

[14_spsc_live_handoff_queue_claim.cpp](../../examples/components/14_spsc_live_handoff_queue_claim.cpp)

Claim the head; cancel to retain work that cannot yet be processed.

examples/components/14_spsc_live_handoff_queue_claim.cpp

C++ · 155 lines

[15_handoff_queue.cpp](../../examples/components/15_handoff_queue.cpp)

Built-in consumers process handoffs and retry rejected work.

examples/components/15_handoff_queue.cpp

C++ · 96 lines

[16_handoff_queue_mpmc.cpp](../../examples/components/16_handoff_queue_mpmc.cpp)

Concurrent producers and consumers share a handoff queue.

examples/components/16_handoff_queue_mpmc.cpp

C++ · 87 lines

[example_support.hpp](../../examples/example_support.hpp)

Shared example checks, parsing and helpers.

examples/example_support.hpp

C++ · 54 lines

## Components

Read these headers for lower-level behavior.

[handoff_queue.hpp](../../src/base_components/HandoffQueue/handoff_queue.hpp)

Resource handoff with multiple concurrency modes and consumer workers.

src/base_components/HandoffQueue/handoff_queue.hpp

C++ · 102 lines

[spsc_live_handoff_queue.hpp](../../src/base_components/LiveHandoffQueue/spsc_live_handoff_queue.hpp)

SPSC resource handoff with access to data still being produced.

src/base_components/LiveHandoffQueue/spsc_live_handoff_queue.hpp

C++ · 353 lines

[warm_pool.hpp](../../src/base_components/WarmPool/warm_pool.hpp)

Resource creation, borrowing, return and capacity control.

src/base_components/WarmPool/warm_pool.hpp

C++ · 375 lines

## Benchmark scripts

Execution, parameter selection and saved results.

[test_per_write_block_size_only_writer.py](../../benchmarks/block_size/test_per_write_block_size_only_writer.py)

Complete source referenced by the documentation.

benchmarks/block_size/test_per_write_block_size_only_writer.py

Python · 254 lines

[test_per_writer_block_size.py](../../benchmarks/block_size/test_per_writer_block_size.py)

Complete source referenced by the documentation.

benchmarks/block_size/test_per_writer_block_size.py

Python · 324 lines

[run_burst_empty.py](../../benchmarks/burst/run_burst_empty.py)

Complete source referenced by the documentation.

benchmarks/burst/run_burst_empty.py

Python · 158 lines

[test_hot_path_cache.py](../../benchmarks/hot_path/test_hot_path_cache.py)

Complete source referenced by the documentation.

benchmarks/hot_path/test_hot_path_cache.py

Python · 285 lines

[bootstrap_spsc.py](../../benchmarks/spsc/bootstrap_spsc.py)

Complete source referenced by the documentation.

benchmarks/spsc/bootstrap_spsc.py

Python · 55 lines

[plot_release.py](../../benchmarks/spsc/plot_release.py)

Plot saved results without rerunning benchmarks.

benchmarks/spsc/plot_release.py

Python · 70 lines

[run_release_benchmarks.py](../../benchmarks/spsc/run_release_benchmarks.py)

Run configured measurements, validate payloads and save all records.

benchmarks/spsc/run_release_benchmarks.py

Python · 220 lines

[tune_spsc.py](../../benchmarks/spsc/tune_spsc.py)

Complete source referenced by the documentation.

benchmarks/spsc/tune_spsc.py

Python · 738 lines

[run.py](../performance_data/latency32_investigation_20261010/run.py)

Historical experiment script, preserving its measured logic.

docs/performance_data/latency32_investigation_20261010/run.py

Python · 43 lines

[restyle_comparison.py](../performance_data/plateau_compare_256m_zero_20261012/restyle_comparison.py)

Historical experiment script, preserving its measured logic.

docs/performance_data/plateau_compare_256m_zero_20261012/restyle_comparison.py

Python · 312 lines

[select_weighted.py](../performance_data/plateau_compare_256m_zero_20261012/weighted_common_config_20261012/select_weighted.py)

Historical experiment script, preserving its measured logic.

docs/performance_data/plateau_compare_256m_zero_20261012/weighted_common_config_20261012/select_weighted.py

Python · 185 lines

## Assembly

Actual instructions saved during performance diagnosis.

[business_original.asm](../performance_data/burst_instruction_1m_20261012/business_original.asm)

Saved assembly with line navigation.

docs/performance_data/burst_instruction_1m_20261012/business_original.asm

Assembly · 45 lines

[push_codegen.s](../performance_data/rigtorp_prefetch_20261011/push_codegen.s)

Saved assembly with line navigation.

docs/performance_data/rigtorp_prefetch_20261011/push_codegen.s

Assembly · 66 lines

## Build integration

CMake configuration for application integration.

[CMakeLists.txt](../../test/integration/CMakeLists.txt)

Check vendored, FetchContent and installed-package integration.

test/integration/CMakeLists.txt

CMake · 32 lines

## Technical notes

Saved diagnostic notes and experiments.

[README.txt](../performance_data/twobufqueue_8b_regression_20261011/README.txt)

Saved technical notes and experiments.

docs/performance_data/twobufqueue_8b_regression_20261011/README.txt

Text · 35 lines

TwoBufQueue · C++ · Apache-2.0 [Overview](../../README.md)
