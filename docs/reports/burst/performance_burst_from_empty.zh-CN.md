[English](https://xiyang1314.github.io/TwoBufQueue/docs/reports/burst/performance_burst_from_empty.html) · [中文](https://xiyang1314.github.io/TwoBufQueue/docs/reports/burst/performance_burst_from_empty.zh-CN.html)

# 连续发送一批消息，对业务性能产生什么影响

比较几种消息队列在突发连续发送后，对生产者线程业务执行性能的影响。

三次独立运行 每批从零积压开始

## 模拟场景

在通常人们使用 SPSC 队列的时候会遇到一个问题，就是时不时会出现消费者线程因为突发原因而产生消息积压，从而拖累生产者线程。这时处理方式一般是加大队列的缓存容量。但是，缓存队列容量要分配多少才能避免这种偶然突发而造成的背压，这是个令人头痛的问题。而且即便扩大缓存容量，突发地持续发送一批消息也会影响生产者线程 L1、L2 或者 L3 的缓存命中率，从而影响热路径性能。这对于比如机器人、高频交易等需要热路径极致低延时的应用来说影响会比较大。而双层缓存能比较好地解决这类问题，这正是 TwoBufQueue 的设计思路。

这个测试模拟的是突发持续地发送一批消息，让消息队列产生消息积压，看看几种队列在这种情况下对业务产生的性能影响状况。

## 测试流程

正常执行业务函数 **→** 循环发送一批消息 **→** 立刻执行业务函数

生产者线程先循环执行业务函数，执行完后循环发送一批消息到消息队列，等发送完再立刻执行同样的业务函数。

这里统计的是发送完消息后执行业务函数的耗时，以及它相对于正常执行同样业务函数耗时的比值。每批从零积压开始，测量后在计时外排空，再开始下一批；消费者与复制线程持续运行。

## 性能测试

正常执行业务函数耗时：约 13 ns/次

表中耗时为发送后每组 16 次业务调用平均耗时的 **p50**，再对三轮取平均。**比值 = 发送后耗时 ÷ 正常耗时（13 ns/次）**；数值越低越好。

每组对比四个队列，柱条越短表示业务耗时越低。每张图按本组最大耗时缩放，精确数值标在右侧，单位为 ns/次。

每批 2 MiB 业务耗时 · p50

TwoBufQueue 15.6

ReaderWriterQueue 39.1

rigtorp::SPSCQueue 15.6

atomic_queue 43.4

每批 4 MiB 业务耗时 · p50

TwoBufQueue 15.6

ReaderWriterQueue 43.4

rigtorp::SPSCQueue 22.6

atomic_queue 47.7

每批 8 MiB 业务耗时 · p50

TwoBufQueue 15.6

ReaderWriterQueue 46.9

rigtorp::SPSCQueue 43.4

atomic_queue 49.5

每批 16 MiB 业务耗时 · p50

TwoBufQueue 18.2

ReaderWriterQueue 58.2

rigtorp::SPSCQueue 42.5

atomic_queue 99.8

| 每批发送量 | 队列 | 耗时(ns/次) | 比值 |
| --- | --- | --- | --- |
| **2 MiB 65,536 条消息** | **TwoBufQueue** | **15.6** | **1.20×** |
| 2 MiB 65,536 条消息 | ReaderWriterQueue | 39.1 | 3.00× |
| 2 MiB 65,536 条消息 | rigtorp::SPSCQueue | 15.6 | 1.20× |
| 2 MiB 65,536 条消息 | atomic_queue | 43.4 | 3.34× |
| **4 MiB 131,072 条消息** | **TwoBufQueue** | **15.6** | **1.20×** |
| 4 MiB 131,072 条消息 | ReaderWriterQueue | 43.4 | 3.34× |
| 4 MiB 131,072 条消息 | rigtorp::SPSCQueue | 22.6 | 1.74× |
| 4 MiB 131,072 条消息 | atomic_queue | 47.7 | 3.67× |
| **8 MiB 262,144 条消息** | **TwoBufQueue** | **15.6** | **1.20×** |
| 8 MiB 262,144 条消息 | ReaderWriterQueue | 46.9 | 3.61× |
| 8 MiB 262,144 条消息 | rigtorp::SPSCQueue | 43.4 | 3.34× |
| 8 MiB 262,144 条消息 | atomic_queue | 49.5 | 3.81× |
| **16 MiB 524,288 条消息** | **TwoBufQueue** | **18.2** | **1.40×** |
| 16 MiB 524,288 条消息 | ReaderWriterQueue | 58.2 | 4.47× |
| 16 MiB 524,288 条消息 | rigtorp::SPSCQueue | 42.5 | 3.27× |
| 16 MiB 524,288 条消息 | atomic_queue | 99.8 | 7.68× |

正常耗时来自同批无队列对照，精确值保留在原始数据中；比值统一使用上方显示的整数参考值。这里测量的是业务执行成本，不是单条消息从发送到接收的延时。

## 结论

- 总体上，连续发送的消息量越大，对热路径业务性能的影响越大。
- 本次测量中，TwoBufQueue 采用双层缓存设计，业务耗时从正常的约 13 ns/次增大到 15.6–18.2 ns/次，最高不到 20 ns/次，对热路径业务的影响较小。
- 其他几个队列采用单层缓存设计，在较大发送量下，业务耗时增长更明显；2 MiB 时 rigtorp 与 TwoBufQueue 持平。

## 注意

这里面向的是允许批量交接、允许一批消息稍后处理的场景。生产者是热路径线程，消费者是辅助线程，目标是尽量减少队列对生产者后续业务的影响；在本次配置下，TwoBufQueue 更适合这个目标。本次测试的消费者持续运行，并没有等整批发送完才开始接收。

如果优先追求每条消息从发送到接收的时间极短，单层 SPSC 队列可避免额外的第二层复制，更有利于这个目标。

### 基本参数



消息长度 **32 B** 完整内容随序号变化

每批发送量 **2–16 MiB** 2、4、8、16 MiB

业务数据表 **48 KiB** 每次访问并更新 8 个节点

TwoBufQueue 第一层 **16 KiB** 4 × 4 KiB

第二层 / 原生槽位 **256 MiB** 元数据另计

每批起始积压 **0 B** 所有窗口均校验

回调统一执行 64 次计算循环；每种发送量每轮 2,500 个窗口，采集 10,000 组业务耗时，独立运行 3 轮。分配、缓冲页预触碰、业务预热和排空不计入业务耗时。

### 实际剩余消息

所有窗口的起始积压均为 0，准备消息数均为 0。以下均值在发送和立即业务测量后采集，消费者期间持续处理：

- 每批 2 MiB：TwoBufQueue 1.94 MiB；ReaderWriterQueue 1.39 MiB；rigtorp::SPSCQueue 1.10 MiB；atomic_queue 1.88 MiB
- 每批 4 MiB：TwoBufQueue 3.88 MiB；ReaderWriterQueue 2.87 MiB；rigtorp::SPSCQueue 2.16 MiB；atomic_queue 3.75 MiB
- 每批 8 MiB：TwoBufQueue 7.77 MiB；ReaderWriterQueue 5.96 MiB；rigtorp::SPSCQueue 4.28 MiB；atomic_queue 7.54 MiB
- 每批 16 MiB：TwoBufQueue 15.53 MiB；ReaderWriterQueue 12.64 MiB；rigtorp::SPSCQueue 8.48 MiB；atomic_queue 15.06 MiB

### 测试环境



硬件

Apple Silicon

处理器

Apple Silicon (arm64)

内存

0 GiB

操作系统

macOS-14.4.1-arm64-arm-64bit

编译环境

Clang 18.1.8 · C++20 · Release · -O3 -DNDEBUG

线程调度

生产者高 QoS，未硬绑核；复制和消费者线程不绑核

计时方式

mach_absolute_time · 刻度约 41.7 ns

内存池深度统计

OFF

## 参数明细与复现

### 测量口径与复现命令



起始状态：prefill=false、drain_before_burst=true。数据池、消息长度、业务工作集、回调循环、消费者调度参数与原积压实验固定参数一致；本次测量 2、4、8、16 MiB。原生 SPSC API 与上游源码保持原样。TwoBufQueue 多一个复制线程。

```
python3 benchmarks/burst/run_burst_empty.py --profile docs/performance_data/burst_from_empty_20261013/selected_config.json
```

[完整初始化参数](../../performance_data/burst_from_empty_20261013/selected_config.json) · [命令、环境与源码哈希](../../performance_data/burst_from_empty_20261013/workflow.json) · [完整统计及每轮原值](../../performance_data/burst_from_empty_20261013/formal/report_results.json)

### 原始样本



- [baseline · 第 1 轮](../../performance_data/burst_from_empty_20261013/formal/raw/48k_baseline_run1.json.gz)
- [baseline · 第 2 轮](../../performance_data/burst_from_empty_20261013/formal/raw/48k_baseline_run2.json.gz)
- [baseline · 第 3 轮](../../performance_data/burst_from_empty_20261013/formal/raw/48k_baseline_run3.json.gz)
- [TwoBufQueue · 第 1 轮](../../performance_data/burst_from_empty_20261013/formal/raw/48k_TwoBufQueue_run1.json.gz)
- [TwoBufQueue · 第 2 轮](../../performance_data/burst_from_empty_20261013/formal/raw/48k_TwoBufQueue_run2.json.gz)
- [TwoBufQueue · 第 3 轮](../../performance_data/burst_from_empty_20261013/formal/raw/48k_TwoBufQueue_run3.json.gz)
- [ReaderWriterQueue · 第 1 轮](../../performance_data/burst_from_empty_20261013/formal/raw/48k_ReaderWriterQueue_run1.json.gz)
- [ReaderWriterQueue · 第 2 轮](../../performance_data/burst_from_empty_20261013/formal/raw/48k_ReaderWriterQueue_run2.json.gz)
- [ReaderWriterQueue · 第 3 轮](../../performance_data/burst_from_empty_20261013/formal/raw/48k_ReaderWriterQueue_run3.json.gz)
- [rigtorp · 第 1 轮](../../performance_data/burst_from_empty_20261013/formal/raw/48k_rigtorp_run1.json.gz)
- [rigtorp · 第 2 轮](../../performance_data/burst_from_empty_20261013/formal/raw/48k_rigtorp_run2.json.gz)
- [rigtorp · 第 3 轮](../../performance_data/burst_from_empty_20261013/formal/raw/48k_rigtorp_run3.json.gz)
- [atomic_queue · 第 1 轮](../../performance_data/burst_from_empty_20261013/formal/raw/48k_atomic_queue_run1.json.gz)
- [atomic_queue · 第 2 轮](../../performance_data/burst_from_empty_20261013/formal/raw/48k_atomic_queue_run2.json.gz)
- [atomic_queue · 第 3 轮](../../performance_data/burst_from_empty_20261013/formal/raw/48k_atomic_queue_run3.json.gz)

### 全部参数



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
