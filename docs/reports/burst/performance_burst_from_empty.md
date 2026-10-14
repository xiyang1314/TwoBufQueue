[English](https://xiyang1314.github.io/TwoBufQueue/docs/reports/burst/performance_burst_from_empty.html) · [中文](https://xiyang1314.github.io/TwoBufQueue/docs/reports/burst/performance_burst_from_empty.zh-CN.html)

# How does a continuous message batch affect business performance?

Compare how continuous message bursts affect subsequent business execution in the producer thread.

Three independent runs Every batch starts empty

## Simulated scenario

SPSC queues can encounter brief consumer slowdowns that accumulate messages and delay the producer. Increasing queue capacity is a common response, but choosing enough capacity for occasional bursts is difficult. Even with more capacity, a continuous message batch can affect the producer's L1, L2 or L3 cache behavior and hot-path performance. This matters in robotics, high-frequency trading and other systems requiring very low hot-path latency. Two-layer buffering is TwoBufQueue's approach to this problem.

The test sends a continuous burst, allowing pending messages to accumulate, then compares the effect on subsequent business work across queues.

## Test procedure

Run normal business work **→** Send a continuous message batch **→** Immediately run the same business work

The producer first runs the business function repeatedly, then sends a continuous message batch, and immediately executes the same business function again.

Measure post-send business cost and its ratio to normal execution of the same function. Each batch starts empty; drain after measurement outside timing, then begin the next batch. Consumers and copy workers continue running.

## Performance results

Normal business cost: approximately 13 ns/call

Costs use post-send 16-call business group means: their **p50**, averaged over three runs.**Ratio = post-send cost ÷ normal cost (13 ns/call)**; lower is better.

Each panel compares four queues; shorter bars mean lower business cost. Each panel scales to its own largest value, with exact ns/call values at the right.

Batch: 2 MiB Business cost · p50

TwoBufQueue 15.6

ReaderWriterQueue 39.1

rigtorp::SPSCQueue 15.6

atomic_queue 43.4

Batch: 4 MiB Business cost · p50

TwoBufQueue 15.6

ReaderWriterQueue 43.4

rigtorp::SPSCQueue 22.6

atomic_queue 47.7

Batch: 8 MiB Business cost · p50

TwoBufQueue 15.6

ReaderWriterQueue 46.9

rigtorp::SPSCQueue 43.4

atomic_queue 49.5

Batch: 16 MiB Business cost · p50

TwoBufQueue 18.2

ReaderWriterQueue 58.2

rigtorp::SPSCQueue 42.5

atomic_queue 99.8

| Bytes per batch | Queue | Cost (ns/call) | Ratio |
| --- | --- | --- | --- |
| **2 MiB 65,536 messages** | **TwoBufQueue** | **15.6** | **1.20×** |
| 2 MiB 65,536 messages | ReaderWriterQueue | 39.1 | 3.00× |
| 2 MiB 65,536 messages | rigtorp::SPSCQueue | 15.6 | 1.20× |
| 2 MiB 65,536 messages | atomic_queue | 43.4 | 3.34× |
| **4 MiB 131,072 messages** | **TwoBufQueue** | **15.6** | **1.20×** |
| 4 MiB 131,072 messages | ReaderWriterQueue | 43.4 | 3.34× |
| 4 MiB 131,072 messages | rigtorp::SPSCQueue | 22.6 | 1.74× |
| 4 MiB 131,072 messages | atomic_queue | 47.7 | 3.67× |
| **8 MiB 262,144 messages** | **TwoBufQueue** | **15.6** | **1.20×** |
| 8 MiB 262,144 messages | ReaderWriterQueue | 46.9 | 3.61× |
| 8 MiB 262,144 messages | rigtorp::SPSCQueue | 43.4 | 3.34× |
| 8 MiB 262,144 messages | atomic_queue | 49.5 | 3.81× |
| **16 MiB 524,288 messages** | **TwoBufQueue** | **18.2** | **1.40×** |
| 16 MiB 524,288 messages | ReaderWriterQueue | 58.2 | 4.47× |
| 16 MiB 524,288 messages | rigtorp::SPSCQueue | 42.5 | 3.27× |
| 16 MiB 524,288 messages | atomic_queue | 99.8 | 7.68× |

Normal cost comes from the same no-queue control; exact values remain in raw data. Ratios use the integer reference shown above. This measures business execution cost, not individual-message end-to-end latency.

## Conclusions

- Overall, larger continuous message batches have a greater effect on subsequent hot-path business performance.
- In this experiment, TwoBufQueue's two-layer design increases business cost from about 13 ns/call to 15.6–18.2 ns/call, remaining below 20 ns/call and having a small effect on subsequent hot-path work.
- The other queues use single-layer storage and show larger business-cost increases with larger batches. At 2 MiB, rigtorp and TwoBufQueue tie.

## Notes

The scenario permits batch handoff and later processing of a message batch. The producer is the hot-path thread; the consumer is auxiliary. The goal is to minimize interference with subsequent producer work, and TwoBufQueue is better suited to this goal in the measured configuration. Consumers run continuously here; they do not wait for the entire batch to finish before receiving.

For extremely short time between submitting and receiving each individual message, single-layer SPSC queues avoid the extra layer-2 copy and are better suited to that goal.

### Basic parameters



Message length **32 B** Every payload word varies with the sequence

Bytes per batch **2–16 MiB** 2、4、8、16 MiB

Business working set **48 KiB** Access and update 8 nodes per call

TwoBufQueue layer 1 **16 KiB** 4 × 4 KiB

Layer 2 / native slots **256 MiB** Metadata counted separately

Pending bytes at batch start **0 B** Every window validated

Every callback performs the same 64 calculation iterations. Each batch size has 2,500 windows and 10,000 business timing groups per run, with 3 independent runs. Allocation, buffer prefaulting, business warmup and draining are outside business timing.

### Messages still pending

All windows start with 0 pending messages and 0 prepared backlog. The following means are sampled after sending and immediate business timing, while consumers continue processing:

- Batch: 2 MiB: TwoBufQueue 1.94 MiB; ReaderWriterQueue 1.39 MiB; rigtorp::SPSCQueue 1.10 MiB; atomic_queue 1.88 MiB
- Batch: 4 MiB: TwoBufQueue 3.88 MiB; ReaderWriterQueue 2.87 MiB; rigtorp::SPSCQueue 2.16 MiB; atomic_queue 3.75 MiB
- Batch: 8 MiB: TwoBufQueue 7.77 MiB; ReaderWriterQueue 5.96 MiB; rigtorp::SPSCQueue 4.28 MiB; atomic_queue 7.54 MiB
- Batch: 16 MiB: TwoBufQueue 15.53 MiB; ReaderWriterQueue 12.64 MiB; rigtorp::SPSCQueue 8.48 MiB; atomic_queue 15.06 MiB

### Test environment



Hardware

Apple Silicon

Processor

Apple Silicon (arm64)

Memory

0 GiB

Operating system

macOS-14.4.1-arm64-arm-64bit

Compiler and build

Clang 18.1.8 · C++20 · Release · -O3 -DNDEBUG

Thread scheduling

High producer QoS, without hard affinity; copy and consumer threads are unpinned

Timing

mach_absolute_time · approximately 41.7 ns per tick

Pool-depth statistics

OFF

## Parameters and reproduction

### Measurement definition and reproduction



Starting state: prefill=false, drain_before_burst=true. Pool capacities, message length, business working set, callback iterations and consumer scheduling match the original backlog experiment's fixed settings; this run measures 2, 4, 8 and 16 MiB. Native SPSC APIs and upstream code are unchanged. TwoBufQueue has one additional copy thread.

```
python3 benchmarks/burst/run_burst_empty.py --profile docs/performance_data/burst_from_empty_20261013/selected_config.json
```

[Complete initialization](../../performance_data/burst_from_empty_20261013/selected_config.json) · [Commands, environment and source hashes](../../performance_data/burst_from_empty_20261013/workflow.json) · [Complete statistics and per-run values](../../performance_data/burst_from_empty_20261013/formal/report_results.json)

### Raw samples



- [baseline · Run 1](../../performance_data/burst_from_empty_20261013/formal/raw/48k_baseline_run1.json.gz)
- [baseline · Run 2](../../performance_data/burst_from_empty_20261013/formal/raw/48k_baseline_run2.json.gz)
- [baseline · Run 3](../../performance_data/burst_from_empty_20261013/formal/raw/48k_baseline_run3.json.gz)
- [TwoBufQueue · Run 1](../../performance_data/burst_from_empty_20261013/formal/raw/48k_TwoBufQueue_run1.json.gz)
- [TwoBufQueue · Run 2](../../performance_data/burst_from_empty_20261013/formal/raw/48k_TwoBufQueue_run2.json.gz)
- [TwoBufQueue · Run 3](../../performance_data/burst_from_empty_20261013/formal/raw/48k_TwoBufQueue_run3.json.gz)
- [ReaderWriterQueue · Run 1](../../performance_data/burst_from_empty_20261013/formal/raw/48k_ReaderWriterQueue_run1.json.gz)
- [ReaderWriterQueue · Run 2](../../performance_data/burst_from_empty_20261013/formal/raw/48k_ReaderWriterQueue_run2.json.gz)
- [ReaderWriterQueue · Run 3](../../performance_data/burst_from_empty_20261013/formal/raw/48k_ReaderWriterQueue_run3.json.gz)
- [rigtorp · Run 1](../../performance_data/burst_from_empty_20261013/formal/raw/48k_rigtorp_run1.json.gz)
- [rigtorp · Run 2](../../performance_data/burst_from_empty_20261013/formal/raw/48k_rigtorp_run2.json.gz)
- [rigtorp · Run 3](../../performance_data/burst_from_empty_20261013/formal/raw/48k_rigtorp_run3.json.gz)
- [atomic_queue · Run 1](../../performance_data/burst_from_empty_20261013/formal/raw/48k_atomic_queue_run1.json.gz)
- [atomic_queue · Run 2](../../performance_data/burst_from_empty_20261013/formal/raw/48k_atomic_queue_run2.json.gz)
- [atomic_queue · Run 3](../../performance_data/burst_from_empty_20261013/formal/raw/48k_atomic_queue_run3.json.gz)

### All parameters



```
{
  "business_kib": [
    24,
    48,
    96
  ],
  "bursts_mib": [
    2,
    4,
    8,
    16
  ],
  "runs": 3,
  "selection_repeats": 8,
  "samples_per_run": 10000,
  "callback_candidates": [
    16,
    32,
    64,
    128,
    256
  ],
  "process_timeout": 900,
  "fixed_callback_iterations": 0,
  "selected_business_kib": 48,
  "callback_iterations": 64,
  "final_window_repeats": 2500,
  "writer_pool_total_bytes": 16384,
  "reader_pool_total_bytes": 268435456,
  "native_slot_storage_bytes": 268435456,
  "payload_bytes": 32,
  "business_accesses": 8,
  "business_warm_queries": 512,
  "pre_queries": 16,
  "post_queries": 64,
  "group_queries": 16,
  "quantiles": "perrunnearest-rank,then3runarithmeticmean",
  "selection_rule": "use median-window relative advantages over all burst sizes and runs; among candidates within2%of best score choose most wins, lower run variability, then smaller working set",
  "machine": {
    "cpu": "Apple Silicon (arm64)",
    "os": "macOS-14.4.1-arm64-arm-64bit",
    "architecture": "arm64",
    "producer_qos": "user_interactive",
    "hard_affinity": false,
    "os_version": "14.4.1"
  },
  "regular_apis": {
    "ReaderWriterQueue": "try_enqueue+membercopy",
    "rigtorp": "try_push;noaddedprefetch",
    "atomic_queue": "SPSCtry_push"
  },
  "producer_qos": "user_interactive on macOS",
  "producer_cpu": -1,
  "publish_count": 4294967295,
  "scan_current": false,
  "queue_parameters": {
    "baseline": {
      "transport": "two_buf_queue",
      "mode": "throughput",
      "payload_bytes": 32,
      "messages": 10000000,
      "warmup_messages": 0,
      "callback_iterations": 64,
      "verify_payload": true,
      "prepared_input": false,
      "variable_payload": true,
      "prepared_input_limit_bytes": 268435456,
      "measure_callback_time": false,
      "producer_interval_ns": 0,
      "source_block_size": 4096,
      "source_pool_min": 4,
      "source_pool_max": 4,
      "source_queue_capacity": 16,
      "writer_backpressure": "drop",
      "writer_spin_count": 1000,
      "writer_wait_timeout_ms": 5000,
      "output_block_size": 4096,
      "output_frame_capacity": 4,
      "output_pool_min": 65536,
      "output_pool_max": 65536,
      "output_queue_capacity": 65538,
      "cancel_claim_mode": "tail",
      "output_mode": "spsc",
      "output_reader_mode": "wait_signal",
      "output_reader_count": 1,
      "output_retry_wait_us": 1000,
      "output_enable_stats": false,
      "is_scan_current": false,
      "reader_spin_count": 1000,
      "rescan_interval_ns": 1000000,
      "rescan_max_interval_ns": 1000000,
      "consumer_cpu": -1,
      "producer_cpu": -1,
      "producer_qos": "user_interactive",
      "round_storage_bytes": 214748320,
      "fixed_round_boundaries": true,
      "spsc_buffer_bytes": 268435456,
      "prefault_producer_buffers": true,
      "finish_timeout_ms": 120000,
      "work_wait_min_ms": 1,
      "finish_retry_ms": 1,
      "publish_count": 4294967295,
      "use_write_session": false,
      "rigtorp_write_prefetch": true,
      "readerwriterqueue_copy_after_zero": false,
      "spsc_capacity": 0,
      "retry_yield_every": 1024,
      "spsc_idle_spins": 1000,
      "spsc_idle_sleep_ns": 0,
      "timer": "auto",
      "sample_every": 256,
      "max_samples": 100000,
      "latency_batch_size": 1000,
      "timer_calibration_samples": 16384,
      "call_batch_size": 256,
      "random_seed": 42,
      "timeout_seconds": 900,
      "buffer_budget_bytes": 536870912
    },
    "TwoBufQueue": {
      "transport": "two_buf_queue",
      "mode": "throughput",
      "payload_bytes": 32,
      "messages": 10000000,
      "warmup_messages": 0,
      "callback_iterations": 64,
      "verify_payload": true,
      "prepared_input": false,
      "variable_payload": true,
      "prepared_input_limit_bytes": 268435456,
      "measure_callback_time": false,
      "producer_interval_ns": 0,
      "source_block_size": 4096,
      "source_pool_min": 4,
      "source_pool_max": 4,
      "source_queue_capacity": 16,
      "writer_backpressure": "drop",
      "writer_spin_count": 1000,
      "writer_wait_timeout_ms": 5000,
      "output_block_size": 4096,
      "output_frame_capacity": 4,
      "output_pool_min": 65536,
      "output_pool_max": 65536,
      "output_queue_capacity": 65538,
      "cancel_claim_mode": "tail",
      "output_mode": "spsc",
      "output_reader_mode": "wait_signal",
      "output_reader_count": 1,
      "output_retry_wait_us": 1000,
      "output_enable_stats": false,
      "is_scan_current": false,
      "reader_spin_count": 1000,
      "rescan_interval_ns": 1000000,
      "rescan_max_interval_ns": 1000000,
      "consumer_cpu": -1,
      "producer_cpu": -1,
      "producer_qos": "user_interactive",
      "round_storage_bytes": 214748320,
      "fixed_round_boundaries": true,
      "spsc_buffer_bytes": 268435456,
      "prefault_producer_buffers": true,
      "finish_timeout_ms": 120000,
      "work_wait_min_ms": 1,
      "finish_retry_ms": 1,
      "publish_count": 4294967295,
      "use_write_session": false,
      "rigtorp_write_prefetch": true,
      "readerwriterqueue_copy_after_zero": false,
      "spsc_capacity": 0,
      "retry_yield_every": 1024,
      "spsc_idle_spins": 1000,
      "spsc_idle_sleep_ns": 0,
      "timer": "auto",
      "sample_every": 256,
      "max_samples": 100000,
      "latency_batch_size": 1000,
      "timer_calibration_samples": 16384,
      "call_batch_size": 256,
      "random_seed": 42,
      "timeout_seconds": 900,
      "buffer_budget_bytes": 536870912
    },
    "ReaderWriterQueue": {
      "transport": "readerwriterqueue",
      "mode": "throughput",
      "payload_bytes": 32,
      "messages": 10000000,
      "warmup_messages": 0,
      "callback_iterations": 64,
      "verify_payload": true,
      "prepared_input": false,
      "variable_payload": true,
      "prepared_input_limit_bytes": 268435456,
      "measure_callback_time": false,
      "producer_interval_ns": 0,
      "source_block_size": 4096,
      "source_pool_min": 4,
      "source_pool_max": 4,
      "source_queue_capacity": 16,
      "writer_backpressure": "drop",
      "writer_spin_count": 1000,
      "writer_wait_timeout_ms": 5000,
      "output_block_size": 4096,
      "output_frame_capacity": 4,
      "output_pool_min": 65536,
      "output_pool_max": 65536,
      "output_queue_capacity": 65538,
      "cancel_claim_mode": "tail",
      "output_mode": "spsc",
      "output_reader_mode": "wait_signal",
      "output_reader_count": 1,
      "output_retry_wait_us": 1000,
      "output_enable_stats": false,
      "is_scan_current": false,
      "reader_spin_count": 1000,
      "rescan_interval_ns": 1000000,
      "rescan_max_interval_ns": 1000000,
      "consumer_cpu": -1,
      "producer_cpu": -1,
      "producer_qos": "user_interactive",
      "round_storage_bytes": 214748320,
      "fixed_round_boundaries": true,
      "spsc_buffer_bytes": 268435456,
      "prefault_producer_buffers": true,
      "finish_timeout_ms": 120000,
      "work_wait_min_ms": 1,
      "finish_retry_ms": 1,
      "publish_count": 4294967295,
      "use_write_session": false,
      "rigtorp_write_prefetch": true,
      "readerwriterqueue_copy_after_zero": false,
      "spsc_capacity": 8371713,
      "retry_yield_every": 1024,
      "spsc_idle_spins": 0,
      "spsc_idle_sleep_ns": 12500,
      "timer": "auto",
      "sample_every": 256,
      "max_samples": 100000,
      "latency_batch_size": 1000,
      "timer_calibration_samples": 16384,
      "call_batch_size": 256,
      "random_seed": 42,
      "timeout_seconds": 900,
      "buffer_budget_bytes": 536870912
    },
    "rigtorp": {
      "transport": "rigtorp",
      "mode": "throughput",
      "payload_bytes": 32,
      "messages": 10000000,
      "warmup_messages": 0,
      "callback_iterations": 64,
      "verify_payload": true,
      "prepared_input": false,
      "variable_payload": true,
      "prepared_input_limit_bytes": 268435456,
      "measure_callback_time": false,
      "producer_interval_ns": 0,
      "source_block_size": 4096,
      "source_pool_min": 4,
      "source_pool_max": 4,
      "source_queue_capacity": 16,
      "writer_backpressure": "drop",
      "writer_spin_count": 1000,
      "writer_wait_timeout_ms": 5000,
      "output_block_size": 4096,
      "output_frame_capacity": 4,
      "output_pool_min": 65536,
      "output_pool_max": 65536,
      "output_queue_capacity": 65538,
      "cancel_claim_mode": "tail",
      "output_mode": "spsc",
      "output_reader_mode": "wait_signal",
      "output_reader_count": 1,
      "output_retry_wait_us": 1000,
      "output_enable_stats": false,
      "is_scan_current": false,
      "reader_spin_count": 1000,
      "rescan_interval_ns": 1000000,
      "rescan_max_interval_ns": 1000000,
      "consumer_cpu": -1,
      "producer_cpu": -1,
      "producer_qos": "user_interactive",
      "round_storage_bytes": 214748320,
      "fixed_round_boundaries": true,
      "spsc_buffer_bytes": 268435456,
      "prefault_producer_buffers": true,
      "finish_timeout_ms": 120000,
      "work_wait_min_ms": 1,
      "finish_retry_ms": 1,
      "publish_count": 4294967295,
      "use_write_session": false,
      "rigtorp_write_prefetch": false,
      "readerwriterqueue_copy_after_zero": false,
      "spsc_capacity": 8388603,
      "retry_yield_every": 1024,
      "spsc_idle_spins": 4000,
      "spsc_idle_sleep_ns": 1000,
      "timer": "auto",
      "sample_every": 256,
      "max_samples": 100000,
      "latency_batch_size": 1000,
      "timer_calibration_samples": 16384,
      "call_batch_size": 256,
      "random_seed": 42,
      "timeout_seconds": 900,
      "buffer_budget_bytes": 536870912
    },
    "atomic_queue": {
      "transport": "atomic_queue",
      "mode": "throughput",
      "payload_bytes": 32,
      "messages": 10000000,
      "warmup_messages": 0,
      "callback_iterations": 64,
      "verify_payload": true,
      "prepared_input": false,
      "variable_payload": true,
      "prepared_input_limit_bytes": 268435456,
      "measure_callback_time": false,
      "producer_interval_ns": 0,
      "source_block_size": 4096,
      "source_pool_min": 4,
      "source_pool_max": 4,
      "source_queue_capacity": 16,
      "writer_backpressure": "drop",
      "writer_spin_count": 1000,
      "writer_wait_timeout_ms": 5000,
      "output_block_size": 4096,
      "output_frame_capacity": 4,
      "output_pool_min": 65536,
      "output_pool_max": 65536,
      "output_queue_capacity": 65538,
      "cancel_claim_mode": "tail",
      "output_mode": "spsc",
      "output_reader_mode": "wait_signal",
      "output_reader_count": 1,
      "output_retry_wait_us": 1000,
      "output_enable_stats": false,
      "is_scan_current": false,
      "reader_spin_count": 1000,
      "rescan_interval_ns": 1000000,
      "rescan_max_interval_ns": 1000000,
      "consumer_cpu": -1,
      "producer_cpu": -1,
      "producer_qos": "user_interactive",
      "round_storage_bytes": 214748320,
      "fixed_round_boundaries": true,
      "spsc_buffer_bytes": 268435456,
      "prefault_producer_buffers": true,
      "finish_timeout_ms": 120000,
      "work_wait_min_ms": 1,
      "finish_retry_ms": 1,
      "publish_count": 4294967295,
      "use_write_session": false,
      "rigtorp_write_prefetch": true,
      "readerwriterqueue_copy_after_zero": false,
      "spsc_capacity": 8388608,
      "retry_yield_every": 1024,
      "spsc_idle_spins": 2000,
      "spsc_idle_sleep_ns": 10000,
      "timer": "auto",
      "sample_every": 256,
      "max_samples": 100000,
      "latency_batch_size": 1000,
      "timer_calibration_samples": 16384,
      "call_batch_size": 256,
      "random_seed": 42,
      "timeout_seconds": 900,
      "buffer_budget_bytes": 536870912
    }
  },
  "build": {
    "BRQ_ENABLE_POOL_DEPTH_STATS": "OFF",
    "CMAKE_BUILD_TYPE": "Release",
    "CMAKE_CXX_COMPILER": "/opt/homebrew/opt/llvm@18/bin/clang++",
    "CMAKE_CXX_FLAGS": "",
    "CMAKE_CXX_FLAGS_RELEASE": "-O3 -DNDEBUG",
    "CMAKE_CXX_COMPILER_ID": "Clang",
    "CMAKE_CXX_COMPILER_VERSION": "18.1.8"
  },
  "benchmark_parameters": {
    "source_pool_bytes": 16384,
    "business_accesses": 8,
    "warm_queries": 512,
    "query_group": 16,
    "query_groups": 4,
    "target_percent": 80,
    "setup_timeout_ms": 3000,
    "prefill": false,
    "drain_before_burst": true
  },
  "start_mode": "empty_each_burst"
}
```
