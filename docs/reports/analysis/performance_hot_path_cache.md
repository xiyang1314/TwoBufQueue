TwoBufQueue · 热路径小缓存与后台大积压

TWOBUFQUEUE / MEMORY SEPARATION

# 把积压留在后台， · 让写线程使用小数据池

缓存敏感业务与 32 B 消息写入交替执行。回调暂停期间，两层结构继续复制和回收第一层，后台保留待处理消息。

Apple M3 3 次独立运行取平均 高 QoS · 未硬绑核 每条消息校验

TwoBufQueue 写线程侧数据池

512 KiB

另有固定 256 MiB 后台池

写线程侧数据区上限对比

约 512× 更小

开源对照的数据区约 256 MiB

主场景 · 对本次最快开源对照

业务吞吐 +2.9%

67.51 M 次/s；对照 rigtorp::SPSCQueue

## 优势在哪里：后台积压不要求扩大第一层

**TwoBufQueue**

业务线程 → 512 KiB 第一层 · 复制线程 → 256 MiB 第二层 → 回调线程

**原生 SPSC 对照**

业务线程 → 约 256 MiB 队列数据区 · 消费线程 → 回调

每轮最多写入 6,710,886 条、约 204.8 MiB。主场景暂停回调，写线程先完成本轮，随后放开回调并排空。全部实现使用相同消息条数、32 B 数据和业务逻辑。

**在放开回调前的统计点，TwoBufQueue 已把约 99.9% 的正式消息字节复制到第二层；6 / 6 个正式轮次确认回调处于暂停状态。** 数据积压在后台，第一层继续循环复用。

“写线程侧数据区”是写线程会使用的队列数据缓冲上限，**不等于 L1 占用、实际活跃缓存行或进程 RSS**。TwoBufQueue 总数据池为 256 MiB + 512 KiB，原生队列约 256 MiB；框架、块描述符和业务工作集另计。

## 主展示：可控后台积压

私有业务工作集 96 KiB，每次执行 8 次依赖读取与更新，之后写入一条 32 B 消息。绿色整行是 TwoBufQueue。

| 实现 | 业务吞吐 · M次/s ↑ | p50 · ns/次 ↓ | p99 · ns/次 ↓ | p999 · ns/次 ↓ | 背压% |
| --- | --- | --- | --- | --- | --- |
| 业务基线（无队列） | 67.38 | 14.50 | 21.58 | 32.61 | 0%0 / 30,000,000 |
| **TwoBufQueue** | **67.51** | **14.74** | **22.17** | **56.13** | **0%0 / 30,000,000** |
| ReaderWriterQueue | 60.02 | 15.57 | 23.19 | 27.76 | 0%0 / 30,000,000 |
| rigtorp::SPSCQueue | 65.59 | 14.94 | 20.85 | 36.78 | 0%0 / 30,000,000 |
| atomic_queue | 49.84 | 20.56 | 37.36 | 86.40 | 0%0 / 30,000,000 |

**延时口径：** 计时组包含连续 1,000 次“业务计算＋消息生成＋队列写入”，用组耗时 ÷ 1,000 得到 ns/次，再统计 p50/p99/p999。它是组内平均耗时的分位数，**不是单次 write 延时，也不是消息到达回调的延时**。吞吐使用独立无逐条计时的运行；轮末交接与排空不计入热路径时间。

### 主场景的三次独立吞吐值



| 实现 | 第1次 M次/s | 第2次 M次/s | 第3次 M次/s | 变异系数 |
| --- | --- | --- | --- | --- |
| 业务基线（无队列） | 67.32 | 66.46 | 68.35 | 1.40% |
| **TwoBufQueue** | **67.37** | **68.10** | **67.08** | **0.78%** |
| ReaderWriterQueue | 60.46 | 59.47 | 60.13 | 0.84% |
| rigtorp::SPSCQueue | 64.43 | 68.07 | 64.28 | 3.27% |
| atomic_queue | 50.44 | 51.46 | 47.61 | 4.00% |

## 补充对照：真实慢回调

所有队列每条回调执行同一个 16 次计算循环；复制与回调在 TwoBufQueue 中仍分属两个线程。

| 实现 | 业务吞吐 · M次/s ↑ | p50 · ns/次 ↓ | p99 · ns/次 ↓ | p999 · ns/次 ↓ | 背压% |
| --- | --- | --- | --- | --- | --- |
| 业务基线（无队列） | 67.38 | 14.50 | 21.58 | 32.61 | 0%0 / 30,000,000 |
| **TwoBufQueue** | **57.73** | **17.19** | **40.76** | **92.25** | **0%0 / 30,000,000** |
| ReaderWriterQueue | 54.46 | 15.81 | 23.83 | 41.04 | 0%0 / 30,000,000 |
| rigtorp::SPSCQueue | 62.98 | 15.03 | 30.51 | 107.60 | 0%0 / 30,000,000 |
| atomic_queue | 46.30 | 21.25 | 31.43 | 80.39 | 0%0 / 30,000,000 |

## 补充对照：即时消费

回调计算循环次数为 0；条数、FIFO 和消息内容校验仍执行。

| 实现 | 业务吞吐 · M次/s ↑ | p50 · ns/次 ↓ | p99 · ns/次 ↓ | p999 · ns/次 ↓ | 背压% |
| --- | --- | --- | --- | --- | --- |
| 业务基线（无队列） | 67.38 | 14.50 | 21.58 | 32.61 | 0%0 / 30,000,000 |
| **TwoBufQueue** | **58.32** | **17.35** | **21.82** | **34.38** | **0%0 / 30,000,000** |
| ReaderWriterQueue | 62.51 | 15.76 | 21.68 | 34.60 | 0%0 / 30,000,000 |
| rigtorp::SPSCQueue | 60.05 | 16.57 | 22.06 | 41.33 | 0%0 / 30,000,000 |
| atomic_queue | 30.47 | 33.50 | 42.28 | 63.85 | 0%0 / 30,000,000 |

## 第一层变化，第二层保持 256 MiB

同一主场景，普通 write，完整块交接；源池固定 4 块。以下所有候选都保留，主展示取四项加权成绩与最高值相差不超过 2% 的候选中最小的写侧池，并要求相邻候选支持；筛选后另做三次独立对比。

| Writer 块 | 写侧池 | Reader 块 | 业务吞吐 M次/s | p50 ns/次 | p99 ns/次 | p999 ns/次 | 背压% |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 8 KiB | 32 KiB | 8 KiB | 53.04 | 18.13 | 24.50 | 47.24 | 0% |
| 16 KiB | 64 KiB | 16 KiB | 55.37 | 17.86 | 22.94 | 41.65 | 0% |
| 32 KiB | 128 KiB | 32 KiB | 60.26 | 16.62 | 21.06 | 45.86 | 0% |
| 64 KiB | 256 KiB | 64 KiB | 65.02 | 15.06 | 19.33 | 29.10 | 0% |
| **128 KiB** | **512 KiB** | **128 KiB** | **68.07** | **14.47** | **19.10** | **31.19** | **0%** |
| 512 KiB | 2,048 KiB | 512 KiB | 67.46 | 14.44 | 19.38 | 26.78 | 0% |
| 2,048 KiB | 8,192 KiB | 2,048 KiB | 67.07 | 14.75 | 19.18 | 25.18 | 0% |

## 纯计算对照

保持相同消息与后台暂停，仅把私有表读取改为 8 次整数计算。原生队列的复制选项在纯计算负载下另行复测后选择吞吐较好的选项，避免复用缓存敏感场景的选项造成失真。这个对照不能用于证明业务数据的缓存保护收益。

| 实现 | 业务吞吐 · M次/s ↑ | p50 · ns/次 ↓ | p99 · ns/次 ↓ | p999 · ns/次 ↓ | 背压% |
| --- | --- | --- | --- | --- | --- |
| 业务基线（无队列） | 70.59 | 14.44 | 21.00 | 38.71 | 0%0 / 30,000,000 |
| **TwoBufQueue** | **66.71** | **15.04** | **19.29** | **22.93** | **0%0 / 30,000,000** |
| ReaderWriterQueue | 59.30 | 16.56 | 26.08 | 145.40 | 0%0 / 30,000,000 |
| rigtorp::SPSCQueue | 35.22 | 29.22 | 34.07 | 145.21 | 0%0 / 30,000,000 |
| atomic_queue | 51.00 | 18.64 | 23.72 | 31.06 | 0%0 / 30,000,000 |

### 纯计算的复制选项与初始化参数



| 实现 / 复制选项 | 业务吞吐 M次/s | p50 ns/次 | p99 ns/次 | p999 ns/次 |
| --- | --- | --- | --- | --- |
| ReaderWriterQueue · zero_then_copy=True | 59.30 | 16.56 | 26.08 | 145.40 |
| ReaderWriterQueue · zero_then_copy=False | 23.05 | 39.53 | 51.69 | 64.50 |
| rigtorp::SPSCQueue · write_prefetch=True | 35.22 | 29.22 | 34.07 | 145.21 |
| rigtorp::SPSCQueue · write_prefetch=False | 18.32 | 52.57 | 66.14 | 78.54 |

### 业务基线（无队列） · 纯计算完整参数



```
{
  "business_kernel": "arithmetic",
  "business_accesses": 8,
  "effective_business_table_bytes": 0,
  "queue": {
    "transport": "two_buf_queue",
    "mode": "throughput",
    "payload_bytes": 32,
    "messages": 10000000,
    "warmup_messages": 0,
    "callback_iterations": 0,
    "verify_payload": true,
    "prepared_input": false,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": false,
    "producer_interval_ns": 0,
    "source_block_size": 65536,
    "source_pool_min": 4,
    "source_pool_max": 4,
    "source_queue_capacity": 16,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 65536,
    "output_frame_capacity": 4,
    "output_pool_min": 4096,
    "output_pool_max": 4096,
    "output_queue_capacity": 4098,
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
    "round_storage_bytes": 214748364,
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
    "timeout_seconds": 180,
    "buffer_budget_bytes": 536870912
  }
}
```

### TwoBufQueue · 纯计算完整参数



```
{
  "business_kernel": "arithmetic",
  "business_accesses": 8,
  "effective_business_table_bytes": 0,
  "queue": {
    "transport": "two_buf_queue",
    "mode": "throughput",
    "payload_bytes": 32,
    "messages": 10000000,
    "warmup_messages": 0,
    "callback_iterations": 0,
    "verify_payload": true,
    "prepared_input": false,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": false,
    "producer_interval_ns": 0,
    "source_block_size": 131072,
    "source_pool_min": 4,
    "source_pool_max": 4,
    "source_queue_capacity": 16,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 131072,
    "output_frame_capacity": 4,
    "output_pool_min": 2048,
    "output_pool_max": 2048,
    "output_queue_capacity": 2050,
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
    "round_storage_bytes": 214748364,
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
    "timeout_seconds": 180,
    "buffer_budget_bytes": 536870912
  }
}
```

### ReaderWriterQueue · 纯计算完整参数



```
{
  "business_kernel": "arithmetic",
  "business_accesses": 8,
  "effective_business_table_bytes": 0,
  "queue": {
    "transport": "readerwriterqueue",
    "mode": "throughput",
    "payload_bytes": 32,
    "messages": 10000000,
    "warmup_messages": 0,
    "callback_iterations": 0,
    "verify_payload": true,
    "prepared_input": false,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": false,
    "producer_interval_ns": 0,
    "source_block_size": 65536,
    "source_pool_min": 4,
    "source_pool_max": 4,
    "source_queue_capacity": 16,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 65536,
    "output_frame_capacity": 4,
    "output_pool_min": 4096,
    "output_pool_max": 4096,
    "output_queue_capacity": 4098,
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
    "round_storage_bytes": 214748364,
    "fixed_round_boundaries": true,
    "spsc_buffer_bytes": 268435456,
    "prefault_producer_buffers": true,
    "finish_timeout_ms": 120000,
    "work_wait_min_ms": 1,
    "finish_retry_ms": 1,
    "publish_count": 4294967295,
    "use_write_session": false,
    "rigtorp_write_prefetch": true,
    "readerwriterqueue_copy_after_zero": true,
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
    "timeout_seconds": 180,
    "buffer_budget_bytes": 536870912
  }
}
```

### rigtorp::SPSCQueue · 纯计算完整参数



```
{
  "business_kernel": "arithmetic",
  "business_accesses": 8,
  "effective_business_table_bytes": 0,
  "queue": {
    "transport": "rigtorp",
    "mode": "throughput",
    "payload_bytes": 32,
    "messages": 10000000,
    "warmup_messages": 0,
    "callback_iterations": 0,
    "verify_payload": true,
    "prepared_input": false,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": false,
    "producer_interval_ns": 0,
    "source_block_size": 65536,
    "source_pool_min": 4,
    "source_pool_max": 4,
    "source_queue_capacity": 16,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 65536,
    "output_frame_capacity": 4,
    "output_pool_min": 4096,
    "output_pool_max": 4096,
    "output_queue_capacity": 4098,
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
    "round_storage_bytes": 214748364,
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
    "timeout_seconds": 180,
    "buffer_budget_bytes": 536870912
  }
}
```

### atomic_queue · 纯计算完整参数



```
{
  "business_kernel": "arithmetic",
  "business_accesses": 8,
  "effective_business_table_bytes": 0,
  "queue": {
    "transport": "atomic_queue",
    "mode": "throughput",
    "payload_bytes": 32,
    "messages": 10000000,
    "warmup_messages": 0,
    "callback_iterations": 0,
    "verify_payload": true,
    "prepared_input": false,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": false,
    "producer_interval_ns": 0,
    "source_block_size": 65536,
    "source_pool_min": 4,
    "source_pool_max": 4,
    "source_queue_capacity": 16,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 65536,
    "output_frame_capacity": 4,
    "output_pool_min": 4096,
    "output_pool_max": 4096,
    "output_queue_capacity": 4098,
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
    "round_storage_bytes": 214748364,
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
    "timeout_seconds": 180,
    "buffer_budget_bytes": 536870912
  }
}
```

## 测试条件与初始化参数

正式每项 1,000 万条，3 次独立进程取算术平均；运行顺序打乱，测试串行执行。固定 256 MiB 第二层，每轮上限为其 80%，不随第一层变化。队列页通过真实管道预热两轮，业务预热 20,000 次。首次分配和预热不计时。

普通 write：publish_write_ptr_count = UINT32_MAX、is_scan_current = false；消息不写长度。源池与输出池在构造时预分配，块满后交接；已写消息在完整块或轮末交接后供复制线程读取。原生队列使用 spsc_* 字段，初始化记录中的 source/output 字段只供公共配置校验，不会构建双层管道。原生队列保留上游源码与原生入队接口，复制选项在这个相同业务负载下复测，选出三次平均吞吐较好的选项，三个缓存敏感回调场景保持该选项；纯计算负载另外筛选原生复制选项。

### 机器、编译器与电源记录



```
{
  "date": "2026-10-12",
  "platform": "macOS-14.4.1-arm64-arm-64bit",
  "machine": "Apple M3",
  "logical_cpus": 8,
  "ram_bytes": 8589934592,
  "performance_l1_data_bytes": 131072,
  "efficiency_l1_data_bytes": 65536,
  "cache_line_bytes": 128,
  "qos": "USER_INTERACTIVE, verified in each process",
  "hard_affinity": false,
  "compiler": "Homebrew clang version 18.1.8",
  "compile_flags": "Release -O3 -DNDEBUG (CMake)",
  "power": "Now drawing from 'AC Power'\n -InternalBattery-0 (id=6226019)\t100%; charged; 0:00 remaining present: true",
  "business_step": "One shared noinline implementation, verified single symbol with llvm-nm",
  "source_snapshot_sha256": {
    "src/.DS_Store": "0691be68b575f4c3222d27f71d06be86a4b30e96cb99ef5404fefbbeff222ed2",
    "src/two_buf_queue.hpp": "cabc2ab36d4c9d3fdfd3a3b6b8b2302824db30aaf8fafa7144fd2e8576de4039",
    "src/write/seq_writer.cpp": "8128732abe1e41c491630a2f65d15858d59bd4ef92dcf45c854f169855003982",
    "src/write/make_queue.hpp": "808415585792bbeba881b0af7beb6c60e9bf16d21b742a7f163775834ad478f7",
    "src/write/writer.hpp": "d81f05a9115f8d182c8ba936263dc979acc7fd343ead03c7c182da885219eec0",
    "src/write/sync_context.hpp": "42f007db1badf5a012bf3071c3c9a77577c052029bae1456b12d7d4647eb4d08",
    "src/write/types.hpp": "ee9185d6ae49801c1a28f8010ba209456935e148116aab1895dbb73c0ecfe828",
    "src/write/seq_writer.hpp": "6763aff97c70daabf3b9ae2a12db079b1ef52453a19f4dbe662040b59ccfd1b9",
    "src/write/writer.cpp": "d3349d9143deadaf3f9242ad3ea25735f9209ec662c695d782878b2a2620fc93",
    "src/write/writer_base.hpp": "04b56920b6708f784a3087c5aab1177f7a460398631d85ac37a722d79ddef27c",
    "src/base_components/block_slice/block_resource_factory.hpp": "ac3122d5267b12119b14ce9c484f6640b6afbd5c713aa3b8fea85bea1bfcfc5f",
    "src/base_components/block_slice/basic_block.hpp": "c1b82d3ad5860b73d2199b31d513a1ace91c67ebec384b7fc093d776940fb178",
    "src/base_components/block_slice/reader_block.hpp": "c87fb4ada08dc09ce1328fc05bfc3ccc9ad1254c4e2c60aee2d64f8cbdd7fb32",
    "src/base_components/block_slice/slice_pool.hpp": "0d68260e3d41b881faf2d57787fd1a992bdd2a34d361b02c69d59104e8e82726",
    "src/base_components/block_slice/block_slices_allocator.hpp": "ef196108b3bd19878a00f3f22cac2a093752a59984fabb0a49c4309c36866d5b",
    "src/base_components/block_slice/writer_block.hpp": "fb9d41b36f88301fcdada460fd390be8f1393173a1e94cc5b8377c923914acc9",
    "src/base_components/block_slice/memory_slice.hpp": "122a2d4180051bef1b1575a76340007e859aea23a4cddce55fcf8c6aaf83b5e5",
    "src/base_components/LiveHandoffQueue/spsc_live_handoff_queue.hpp": "04322881bf4b2a0176bc6c3ef1889f8e23d4f6ab7eaa44c4053fdfe3b3ed549f",
    "src/base_components/HandoffQueue/claim_queue.hpp": "e3dd6e79d3aca4cae96f619104d44763b6ae7f87a5636088731cf1805baea3ea",
    "src/base_components/HandoffQueue/pooled_resource_queue.hpp": "ce12e04b0e5d7870499962e43a377c3dd33385da97bfd460b97a24bbcee2858c",
    "src/base_components/HandoffQueue/handoff_reader.hpp": "d1191417435a515003bfe029d52ee9e643888ae1bc8d610b9abfa075e509dee5",
    "src/base_components/HandoffQueue/queue_ticket.hpp": "3716dbd2c38a0c6cf743be62bda01d334fbfb7a7517f003fb09a82c1d16da6ee",
    "src/base_components/HandoffQueue/ready_index_queue.hpp": "2db96b7e52e6e68f708c61e90f819c69b8b17cc64faab1c82b80bafc8ec0c20b",
    "src/base_components/HandoffQueue/handoff_queue.hpp": "451fac983f6fb8af9feadbdcaf9e93519bfbdb831b37ae0263181b82a9912747",
    "src/base_components/HandoffQueue/handoff_types.hpp": "a877583d4dffc236c5d45d97427faa36c83fd7b51d133042e9ba3690cb57cd5f",
    "src/base_components/HandoffQueue/lock_free_index_queue.hpp": "62b497f6f5d70553a6fdf2b7ea0c9ca645cb4e119c9407a7b1b30ea9e9ca338d",
    "src/base_components/WarmPool/warm_pool.hpp": "cdc88f35642e4c81bfbb9072027fe56d1592180a070bef22afadd6954c0bd641",
    "src/base_components/WarmPool/intrusive_stack.hpp": "912dae641f26b79fecb3f0926aac0e8255c0c67ff0addd4c56d73388faf98bea",
    "src/base_components/WarmPool/depth_stats.hpp": "763d12709fdcf42d81789ca86ae5cdf8d16cae9ea804ffa6c13dd27a3af1aa02",
    "src/base_components/WarmPool/intrusive_queue.hpp": "0b87a1615c000ea6aab805a6ec4443240adeb13d34f218b6d7ba0ecb6aea11ec",
    "src/base_components/WarmPool/resource.hpp": "9adffca45f4e69a6056f83fda03ff0951ff32fd5816fdbf675be92ba309d82a9",
    "src/read/layer2/to_read_block.hpp": "5afac965742b68f299a703b881f8c53dc2b14170ed2d0b81c01fd0ac2215d7d1",
    "src/read/layer2/read_callbacks.hpp": "e90b6705ffdeaecab6f85d5452b5ab61a4799ceabd931def2d7fece221b64121",
    "src/read/layer2/read_block_factory.hpp": "2c7f0a30323b032017ebc5e81bd8cc0efdd2ce3f6fc0e8c2d51174d0771521d2",
    "src/read/layer2/read_block.hpp": "ba24129aa20c9d2643d9b4a8068287395bb51d870289115644d023a343b70402",
    "src/read/read_layer1/read_service.hpp": "4bdfa159a804f4c7e77b1e375c9582e76f306c11758cbbd37248e9b5767c8736",
    "src/read/read_layer1/read_worker.hpp": "4e8a9692b6cd854dec06de332599b4b23ac24651a642cf7b70452694b6fb9608",
    "src/read/read_layer1/read_writer_data_callback.hpp": "537b0c93fcd1c4a3c4bb5e09170f7e0122705faa4652906f096e7bc4395310d3",
    "src/read/read_layer1/types.hpp": "d9f3115daf84f53b9d530ce14d94ed949f51dd105dacce8ef098c3f08461a278",
    "src/read/read_layer1/read_item.hpp": "f62cb6548bee70fd09b67d3a829ede5ce6b8bc178ad4d0c25149bfabae53ff0f",
    "src/read/read_layer1/muti_writer/read_service.hpp": "92c9e9d45617c629565aa0933f1edabfe3112257cd501edf8df83321ed9127e7",
    "src/read/read_layer1/muti_writer/read_worker.hpp": "d760a8db4dd3db051c363f4cfbff426ec97c40a031c31938de25e4c3c187ab3a",
    "src/read/read_layer1/single_writer/read_service.hpp": "55a9fa356b339b9151d3ec5b0e8836c4d75bb6477f1b4e26aa19f4f759b5be85",
    "src/read/read_layer1/single_writer/read_worker.hpp": "ebd883583ba6f0e8f92c13200b852023f497ab7e8cbb277539d07ab79545c198",
    "src/utils/block_diagnostics.hpp": "d81e6b16dba8ac62c240c52ed3b25bf75e5d7edb6be8573be7ef3be7ba5b2589",
    "src/utils/spsc_queue.hpp": "1cdea11ca6d9168e72aa964fb9f6d9f189d26c150e8ee9a4c2eb541cafe294c8",
    "src/utils/plain_value.hpp": "fdfc7afe7072c7c452631da0f44dc22657fcc0cbde57a5a8c8f4ff43fb5058cd",
    "src/utils/spsc_pointer_queue.hpp": "60eb5de77c466a4185e5ed584d993500ba593ee6adb3c7ff754b9e77e932b861",
    "src/utils/auto_reset_event.cpp": "8d37d360311652a12e3dc5372f5bacc3a0763f27b06270b4a9dd32cc200b6f2d",
    "src/utils/no_lock.hpp": "c2c5eff008255c2a39d86b95a7af7023252c31d4da23b948d78fb9f213350b35",
    "src/utils/brqHelp.hpp": "f3b960d533e6bb6d82b79d0e2a90ed3843e1675cfd81a999cc3909ef3432ba94",
    "src/utils/fast_time.cpp": "aeb65319574b99434dfa995324613486adfa7f173a518a58187e7b52e24dfe28",
    "src/utils/auto_reset_event.hpp": "10ac49eca50d3850cae94420799fc0f6b342822e4e3fef46b1dd16d19b472979",
    "src/utils/brqHelp.cpp": "041b72c92ac6606727da275dd4b123039e1a0e1d99be2674bafc8ff657cb2c9f",
    "src/utils/fast_time.hpp": "bc75837066e7e11aed57a2365facad48d3c9dcecea76fe869b62e50711136f90",
    "src/utils/spin_lock.hpp": "7a0574871f73a84b17dd47f15a2c0c1a447009913695d434d64fd0048d2985d4",
    "src/utils/wait.hpp": "e855d6e0a86375332ba55b43e11a10257ab647a8f07971530a5aa5ed479074a1",
    "src/data/spsc_basic_block.hpp": "4f2168a97b40357b23ba9cb1c16a50241e0ecfb0f82be2ccdabf3a428bd3012d",
    "src/data/data_frame.hpp": "fecb242429f3044c14950ed0f12a22f35de74af73bdd4f839084ac3e5f3c5965",
    "src/data/spsc_read_block.hpp": "c8def8e59324f5980d84e4aa072bc45cfa152b25ce733a4f30e08c5fe4985f65",
    "src/data/spsc_write_block.hpp": "2e6139340c502a36f3ffe15ee59fc8bd1203a663d253e53cf96794440fed0e43",
    "src/data/Untitled": "443d72e4d7be88cb9bcb6e7c3f95b466e89f58ad36dad14b52fbbcf44c3b69ed",
    "src/data/data_batch.hpp": "8673ec49ed45ff511335cd0e853a72875d649b461376613ca5618ac355378bf9"
  },
  "arithmetic_correction_power": "Now drawing from 'AC Power'\n -InternalBattery-0 (id=6226019)\t100%; charged; 0:00 remaining present: true"
}
```

### 业务基线（无队列） · 完整初始化参数



```
{
  "business": {
    "bytes": 98304,
    "accesses": 8,
    "entry_bytes": 64,
    "warmup_steps": 20000,
    "seed": 42
  },
  "scenarios": {
    "paused": {
      "gate": "hold first callback until round writer completes"
    },
    "slow": {
      "callback_iterations": 16
    },
    "fast": {
      "callback_iterations": 0
    }
  },
  "queue": {
    "transport": "two_buf_queue",
    "mode": "throughput",
    "payload_bytes": 32,
    "messages": 10000000,
    "warmup_messages": 0,
    "callback_iterations": 0,
    "verify_payload": true,
    "prepared_input": false,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": false,
    "producer_interval_ns": 0,
    "source_block_size": 65536,
    "source_pool_min": 4,
    "source_pool_max": 4,
    "source_queue_capacity": 16,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 65536,
    "output_frame_capacity": 4,
    "output_pool_min": 4096,
    "output_pool_max": 4096,
    "output_queue_capacity": 4098,
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
    "round_storage_bytes": 214748364,
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
    "timeout_seconds": 180,
    "buffer_budget_bytes": 536870912
  }
}
```

### TwoBufQueue · 完整初始化参数



```
{
  "business": {
    "bytes": 98304,
    "accesses": 8,
    "entry_bytes": 64,
    "warmup_steps": 20000,
    "seed": 42
  },
  "scenarios": {
    "paused": {
      "gate": "hold first callback until round writer completes"
    },
    "slow": {
      "callback_iterations": 16
    },
    "fast": {
      "callback_iterations": 0
    }
  },
  "queue": {
    "transport": "two_buf_queue",
    "mode": "throughput",
    "payload_bytes": 32,
    "messages": 10000000,
    "warmup_messages": 0,
    "callback_iterations": 0,
    "verify_payload": true,
    "prepared_input": false,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": false,
    "producer_interval_ns": 0,
    "source_block_size": 131072,
    "source_pool_min": 4,
    "source_pool_max": 4,
    "source_queue_capacity": 16,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 131072,
    "output_frame_capacity": 4,
    "output_pool_min": 2048,
    "output_pool_max": 2048,
    "output_queue_capacity": 2050,
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
    "round_storage_bytes": 214748364,
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
    "timeout_seconds": 180,
    "buffer_budget_bytes": 536870912
  }
}
```

### ReaderWriterQueue · 完整初始化参数



```
{
  "business": {
    "bytes": 98304,
    "accesses": 8,
    "entry_bytes": 64,
    "warmup_steps": 20000,
    "seed": 42
  },
  "scenarios": {
    "paused": {
      "gate": "hold first callback until round writer completes"
    },
    "slow": {
      "callback_iterations": 16
    },
    "fast": {
      "callback_iterations": 0
    }
  },
  "queue": {
    "transport": "readerwriterqueue",
    "mode": "throughput",
    "payload_bytes": 32,
    "messages": 10000000,
    "warmup_messages": 0,
    "callback_iterations": 0,
    "verify_payload": true,
    "prepared_input": false,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": false,
    "producer_interval_ns": 0,
    "source_block_size": 65536,
    "source_pool_min": 4,
    "source_pool_max": 4,
    "source_queue_capacity": 16,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 65536,
    "output_frame_capacity": 4,
    "output_pool_min": 4096,
    "output_pool_max": 4096,
    "output_queue_capacity": 4098,
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
    "round_storage_bytes": 214748364,
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
    "timeout_seconds": 180,
    "buffer_budget_bytes": 536870912
  }
}
```

### rigtorp::SPSCQueue · 完整初始化参数



```
{
  "business": {
    "bytes": 98304,
    "accesses": 8,
    "entry_bytes": 64,
    "warmup_steps": 20000,
    "seed": 42
  },
  "scenarios": {
    "paused": {
      "gate": "hold first callback until round writer completes"
    },
    "slow": {
      "callback_iterations": 16
    },
    "fast": {
      "callback_iterations": 0
    }
  },
  "queue": {
    "transport": "rigtorp",
    "mode": "throughput",
    "payload_bytes": 32,
    "messages": 10000000,
    "warmup_messages": 0,
    "callback_iterations": 0,
    "verify_payload": true,
    "prepared_input": false,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": false,
    "producer_interval_ns": 0,
    "source_block_size": 65536,
    "source_pool_min": 4,
    "source_pool_max": 4,
    "source_queue_capacity": 16,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 65536,
    "output_frame_capacity": 4,
    "output_pool_min": 4096,
    "output_pool_max": 4096,
    "output_queue_capacity": 4098,
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
    "round_storage_bytes": 214748364,
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
    "timeout_seconds": 180,
    "buffer_budget_bytes": 536870912
  }
}
```

### atomic_queue · 完整初始化参数



```
{
  "business": {
    "bytes": 98304,
    "accesses": 8,
    "entry_bytes": 64,
    "warmup_steps": 20000,
    "seed": 42
  },
  "scenarios": {
    "paused": {
      "gate": "hold first callback until round writer completes"
    },
    "slow": {
      "callback_iterations": 16
    },
    "fast": {
      "callback_iterations": 0
    }
  },
  "queue": {
    "transport": "atomic_queue",
    "mode": "throughput",
    "payload_bytes": 32,
    "messages": 10000000,
    "warmup_messages": 0,
    "callback_iterations": 0,
    "verify_payload": true,
    "prepared_input": false,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": false,
    "producer_interval_ns": 0,
    "source_block_size": 65536,
    "source_pool_min": 4,
    "source_pool_max": 4,
    "source_queue_capacity": 16,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 65536,
    "output_frame_capacity": 4,
    "output_pool_min": 4096,
    "output_pool_max": 4096,
    "output_queue_capacity": 4098,
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
    "round_storage_bytes": 214748364,
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
    "timeout_seconds": 180,
    "buffer_budget_bytes": 536870912
  }
}
```

### 开源复制选项对照（全部保留）



| 实现 / 复制选项 | 业务吞吐 M次/s | p50 ns/次 | p99 ns/次 | p999 ns/次 |
| --- | --- | --- | --- | --- |
| ReaderWriterQueue · zero_then_copy=True | 58.86 | 16.35 | 28.53 | 71.10 |
| ReaderWriterQueue · zero_then_copy=False | 59.82 | 15.79 | 23.96 | 47.86 |
| rigtorp::SPSCQueue · write_prefetch=True | 67.09 | 14.57 | 18.87 | 23.50 |
| rigtorp::SPSCQueue · write_prefetch=False | 66.80 | 14.58 | 20.26 | 37.72 |

## 数据与校验

288 项正式运行、48 项独立全负载校验，全部通过。独立校验逐条比较完整 32 B 业务结果；正式运行核验 FIFO、每条消息的确定字段与整体校验和。每条正式消息均完成回调后才结束该测试。

原始 JSON 包含每次结果、全部参数、精确命令、可执行文件签名、背压条数、轮次、回调时长、页错误和数据校验记录。整数背压%会显示为 0% 的极小比例，表内同时列出精确条数。

结果仅描述这台 M3 上的合成业务与已列明的线程调度条件。主场景主动暂停回调，用于展示积压与写侧数据池的解耦；慢回调、即时消费和纯计算对照一并列出。当前缓存敏感主场景的吞吐优势较小，补充场景还有落后指标；这份测试证明小写侧池与大后台积压可以解耦，尚未证明能显著减少业务缓存干扰。未采集新的硬件 PMU 数据，不将速度差异直接归因于 L1 命中率。

[汇总 JSON](../../performance_data/hot_path_cache_20261012/summary.json) · [全部汇总 CSV](../../performance_data/hot_path_cache_20261012/results.csv) · [复现命令与文件清单](../../performance_data/hot_path_cache_20261012/manifest.json)

生成于 2026-10-12 13:36:24 +0800 · 性能指标来自真实运行，容量优势指队列写侧数据区。
