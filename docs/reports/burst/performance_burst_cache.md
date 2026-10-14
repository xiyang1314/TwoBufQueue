# 消息积压时，热路径业务还能跑多快？

模拟消费较慢、队列已有大量待处理消息的场景，比较几种队列对同一热路径业务的影响。

Apple M3 三次独立运行 高 QoS · 未硬绑核 全部消息校验

## 模拟场景

在通常人们使用spsc队列的时候会遇到一个问题，就是时不时会出现消费者线程因为突发原因而产生任务积压，从而拖累生产者线程。这时处理方式一般是加大队列的缓存容量。但是，缓存队列容量要分配多少才能避免这种偶然突发而造成的背压，这是个令人头痛的问题。而且即便扩大缓存容量，消息积压也会很大程度的影响生产者线程的l1、l2或者l3的缓存命中率，从而影响热路径性能。这对于比如机器人，高频交易等低延时的应用来说影响会比较大。而双层缓存能比较好的解决这类问题，这正是TwoBufQueue的设计思路。

这个测试模拟的是让消息队列产生消息积压，看看几种队列在消息积压时对业务产生的性能影响状况。

## 测试流程

循环发送一批消息 **→** 立即执行业务函数 **↻**

业务函数访问并更新一张 48 KiB 数据表，每次处理 8 个节点。本页展示每批发送量为 2、4、8、16 MiB 的结果，各发送量均重复 2,500 次。复制、消费者线程持续工作，准备积压和最终排空的时间不计入业务耗时。

## 基本参数

消息长度 **32 B** 全部字段随序号变化

每批发送量 **2–16 MiB** 2、4、8、16 MiB

业务数据表 **48 KiB** 每次访问并更新 8 个节点

TwoBufQueue 第一层 **16 KiB** 4 × 4 KiB

第二层 / 开源槽位存储 **256 MiB** 队列元数据另计

待处理消息的积压目标 **约 204.8 MiB** 缓冲容量的 80%

各项发送结束时的平均实际积压约 **198–205 MiB**。回调一直在消费消息，因此实际值会随发送量和处理速度变化。这里统计的是待处理消息的字节数。

每条消息的回调统一执行 **64 次** 计算循环；每项每轮采集 **10,000 组** 业务耗时，独立运行 **3 轮**。所有队列采用同样的消息内容和业务负载。

## 测试环境

硬件

MacBook Pro · Mac15,3

处理器

Apple M3 · 8 核（4 性能核 + 4 能效核）

内存

8 GiB

操作系统

macOS 14.4.1（构建号 23E224） · arm64

编译器

Clang 18.1.8

构建与优化

C++20 · Release · -O3 -DNDEBUG

线程调度

高 QoS · 未硬绑核；复制线程和消费者线程不作硬绑核

计时方式

mach_absolute_time · 计时刻度约 41.7 ns

内存池深度统计

OFF

## 性能测试

无干扰时业务执行时间：约 13 ns/次

表中 **耗时** 指发送消息后立即执行的业务函数耗时，采用 **p50**，反映常见执行成本；数值越小越好。每组计时包含 16 次业务调用，显示单位为 ns/次。

每组对比四个队列，柱条越短表示业务耗时越低。每张图按本组最大耗时缩放，精确数值标在右侧，单位为 ns/次。

每批 2 MiB 业务耗时 · p50

TwoBufQueue 15.6

ReaderWriterQueue 36.5

rigtorp::SPSCQueue 14.8

atomic_queue 41.7

每批 4 MiB 业务耗时 · p50

TwoBufQueue 15.6

ReaderWriterQueue 42.5

rigtorp::SPSCQueue 25.2

atomic_queue 45.1

每批 8 MiB 业务耗时 · p50

TwoBufQueue 16.5

ReaderWriterQueue 50.3

rigtorp::SPSCQueue 49.5

atomic_queue 51.2

每批 16 MiB 业务耗时 · p50

TwoBufQueue 20.0

ReaderWriterQueue 72.0

rigtorp::SPSCQueue 65.1

atomic_queue 106.8

### 业务耗时对比表

| 每批发送量 | 队列 | 耗时(ns/次) |
| --- | --- | --- |
| **2 MiB 65,536 条消息** | **TwoBufQueue** | **15.6** |
| 2 MiB 65,536 条消息 | ReaderWriterQueue | 36.5 |
| 2 MiB 65,536 条消息 | rigtorp::SPSCQueue | 14.8 |
| 2 MiB 65,536 条消息 | atomic_queue | 41.7 |
| **4 MiB 131,072 条消息** | **TwoBufQueue** | **15.6** |
| 4 MiB 131,072 条消息 | ReaderWriterQueue | 42.5 |
| 4 MiB 131,072 条消息 | rigtorp::SPSCQueue | 25.2 |
| 4 MiB 131,072 条消息 | atomic_queue | 45.1 |
| **8 MiB 262,144 条消息** | **TwoBufQueue** | **16.5** |
| 8 MiB 262,144 条消息 | ReaderWriterQueue | 50.3 |
| 8 MiB 262,144 条消息 | rigtorp::SPSCQueue | 49.5 |
| 8 MiB 262,144 条消息 | atomic_queue | 51.2 |
| **16 MiB 524,288 条消息** | **TwoBufQueue** | **20.0** |
| 16 MiB 524,288 条消息 | ReaderWriterQueue | 72.0 |
| 16 MiB 524,288 条消息 | rigtorp::SPSCQueue | 65.1 |
| 16 MiB 524,288 条消息 | atomic_queue | 106.8 |

每轮独立统计业务计时组的 p50，再对三轮结果取平均。无干扰参考值来自同批无队列对照的 p50 平均值，显示为整数；精确数值保留在原始结果中。

## 参数明细与复现

### 测量方法与队列设置



业务与采样

48 KiB 数据表、64 B 节点、随机种子 42；所有实现调用同一个业务函数。每个发送窗口先预热业务数据，发送后立即执行 64 次业务，分成 4 组，每组 16 次。每项每轮 2,500 个窗口，共 10,000 组。

积压准备

目标约 204.8 MiB 待处理消息。发送前预填充并为本批数据预留空间，复制线程及消费者线程持续运行。分配、预触碰缓冲页、准备积压和最终排空均在业务计时区之外。

TwoBufQueue

普通 write，不扫描当前块，publish_write_ptr_count=UINT32_MAX；第一层 4 × 4 KiB，第二层 65,536 × 4 KiB；复制线程 idle_spin_count=1000，重试 yield 间隔=1024。一个生产者线程、一个复制线程、一个消费者线程（执行回调）；尾块在业务计时结束后交接。临时写入失败时重试，全部消息最终处理完成。

开源队列

ReaderWriterQueue 使用 try_enqueue 和普通成员复制；rigtorp::SPSCQueue 使用 try_push，不添加写预取；atomic_queue 使用 try_push 和 SPSC 模式。均采用常规用法，开源队列原实现未修改。

容量口径

比较的是数据存储容量，元数据另计。ReaderWriterQueue 可用 8,372,224 个槽；rigtorp 请求 8,388,603 个槽，加备用槽和 padding 后为 256 MiB；atomic_queue 有 8,388,608 个槽，状态数组另计。TwoBufQueue 使用额外的复制线程。

### 复现命令与原始数据



从项目根目录运行，自动构建、校准、遍历、正式采样并生成本格式报告：

```
python3 benchmarks/burst/run_burst_showcase.py
```

按本次固定参数重新执行：

```
python3 benchmarks/burst/run_burst_showcase.py --replay docs/performance_data/burst_showcase_20261012/selected_config.json
```

使用新的输出目录；中途停止后可传入原来的 --output 并加 --resume 继续。

[全部初始化参数](../../performance_data/burst_showcase_20261012/selected_config.json) · [运行环境与代码哈希](../../performance_data/burst_showcase_20261012/workflow.json) · [完整结果和各轮原值](../../performance_data/burst_showcase_20261012/formal/report_results.json)

消息条数、FIFO、完整 32 B 内容及业务结果均通过校验。原始样本和完整统计保留，本页只展示业务 p50。

- [无队列对照 · 第 1 轮原始样本](../../performance_data/burst_showcase_20261012/formal/raw/48k_baseline_run1.json.gz)
- [无队列对照 · 第 2 轮原始样本](../../performance_data/burst_showcase_20261012/formal/raw/48k_baseline_run2.json.gz)
- [无队列对照 · 第 3 轮原始样本](../../performance_data/burst_showcase_20261012/formal/raw/48k_baseline_run3.json.gz)
- [TwoBufQueue · 第 1 轮原始样本](../../performance_data/burst_showcase_20261012/formal/raw/48k_TwoBufQueue_run1.json.gz)
- [TwoBufQueue · 第 2 轮原始样本](../../performance_data/burst_showcase_20261012/formal/raw/48k_TwoBufQueue_run2.json.gz)
- [TwoBufQueue · 第 3 轮原始样本](../../performance_data/burst_showcase_20261012/formal/raw/48k_TwoBufQueue_run3.json.gz)
- [ReaderWriterQueue · 第 1 轮原始样本](../../performance_data/burst_showcase_20261012/formal/raw/48k_ReaderWriterQueue_run1.json.gz)
- [ReaderWriterQueue · 第 2 轮原始样本](../../performance_data/burst_showcase_20261012/formal/raw/48k_ReaderWriterQueue_run2.json.gz)
- [ReaderWriterQueue · 第 3 轮原始样本](../../performance_data/burst_showcase_20261012/formal/raw/48k_ReaderWriterQueue_run3.json.gz)
- [rigtorp::SPSCQueue · 第 1 轮原始样本](../../performance_data/burst_showcase_20261012/formal/raw/48k_rigtorp_run1.json.gz)
- [rigtorp::SPSCQueue · 第 2 轮原始样本](../../performance_data/burst_showcase_20261012/formal/raw/48k_rigtorp_run2.json.gz)
- [rigtorp::SPSCQueue · 第 3 轮原始样本](../../performance_data/burst_showcase_20261012/formal/raw/48k_rigtorp_run3.json.gz)
- [atomic_queue · 第 1 轮原始样本](../../performance_data/burst_showcase_20261012/formal/raw/48k_atomic_queue_run1.json.gz)
- [atomic_queue · 第 2 轮原始样本](../../performance_data/burst_showcase_20261012/formal/raw/48k_atomic_queue_run2.json.gz)
- [atomic_queue · 第 3 轮原始样本](../../performance_data/burst_showcase_20261012/formal/raw/48k_atomic_queue_run3.json.gz)

### 完整初始化参数与展示范围



保留业务负载、各队列初始化、计时窗口和编译选项；display_bursts_mib 表示本页展示的发送量。原始测量参数与记录可通过上方链接查看。

```
{
  "business_kib": [
    24,
    48,
    96
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
    "producer_qos": "user_interactive",
    "hard_affinity": false
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
    "prefill": true
  },
  "display_bursts_mib": [
    2,
    4,
    8,
    16
  ]
}
```
