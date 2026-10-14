8 MiB / 32 B 写入耗时定位与优化

# 8 MiB 缓存、32 B 消息：p50 为什么偏高

已确认：小块交接及发送期间的并行接收增加成本。仅调整块组织后，写入 p50 降低约 14.2%，与 ReaderWriterQueue 基本追平。

每层池 8 MiB · 回调业务循环 0 · 每次 1,000 万条 · 高 QoS，未硬绑核

## 独立复测结果

两组独立确认，每组各实现跑 5 次取算术平均，下表再对两组均值取平均。没有剔除异常值。正式测试使用原始被测二进制及完整异步链路，第三方实现没有改动。

| 实现 / 配置 | p50 · ns/次 | p99 · ns/次 | p999 · ns/次 | 写入失败 |
| --- | --- | --- | --- | --- |
| **TwoBufQueue · 原 64 KiB 块** | **1.933** | **2.854** | **6.737** | **0** |
| **TwoBufQueue · 8 MiB 块** | **1.658** | **2.821** | **5.333** | **0** |
| ReaderWriterQueue | 1.663 | 8.842 | 11.121 | 0 |
| rigtorp/SPSCQueue | 3.562 | 4.429 | 8.129 | 0 |
| atomic_queue | 8.121 | 28.783 | 39.438 | 0 |

**这些分位数属于每组 1,000 次写入的平均耗时。** 它们不是单次 write 分位数，也不是消息到回调的端到端延时。单组均值的计时刻度为 0.04167 ns。优化后 p50 与 ReaderWriterQueue 相差不到一个刻度，不能宣称稳定、严格领先；两者在多数运行中均为 1.625 ns。

本次 p99、p999 低于三个对照。每次约 9,966 个组均值样本，p999 只由很少的尾部样本决定，不能承诺它长期最低。吞吐没有在这份定位测试中单独测量，因此不将延时测试中的发送速率作为吞吐结果发布。

## 确认了哪些原因

**① 计时组频繁包含换块。** 64 KiB ÷ 32 B = 2,048 条/块，而每个计时组是 1,000 条。额外诊断仅在结束时间戳之后分类，不修改组内写入操作。每次约 4,911 / 9,966 组（49.3%）包含轮首申请块或换块。普通组 p50 约 1.792 ns，含冷操作的组约 1.987 ns。混合分位数不能当成纯 memcpy 成本。

**② 小块使复制与回调更早参与发送阶段。** 正式链路仍为 Writer → 第一层复制线程 → 第二层缓存 → 回调线程。额外“停止接收、发送后直接排空”的诊断中，两边使用相同操作；TwoBufQueue 的 p50 从约 1.967 ns 降为 1.808 ns，但仍未严格领先。这说明接收端并行活动有影响，不能把全部差距归咎于它；该诊断省去了第二层路径，只用于定位，从未作为正式成绩。

**③ 不是 Writer 额外复制了整条消息。** 32 B 热路径机器码中只有一次向目标写入的 SIMD 复制；完成块交接、池操作、通知发生在冷路径。删除部分检查、改变指针写法及循环展开的试验未获得稳定改善，均未合入库实现。

没有采集到硬件缓存与流水线计数，因此不能进一步断言并行影响究竟有多少来自缓存失效、带宽或调度，更不能给出虚构的百分比。

## 本次改动与适用范围

库的 C++ 实现没有因本次调查修改。性能搜索配置把 32 B 的起始单块容量改为 **8 MiB**，两层池各为 **1 块 × 8 MiB**，并加入 2 / 4 / 8 MiB 搜索候选。保留 `is_scan_current=false`、`publish_count=UINT32_MAX`。总池容量、消息数、消息内容、每消息回调方式均不变。

每轮约发送 6.4 MiB，小于一整个 8 MiB 块。因此计时期间不发生自动换块；轮尾按已有逻辑 `finish_block()`，完成第二层复制、回调和排空后再开始下一轮。第二层复制仍实际执行，排空时间按原约定计时范围排除。

**降低写入耗时的代价是更晚交付回调。** 大块、不扫描当前块、只在块完成时发布，意味着轮内消息要等待到轮尾才可见。此配置适用于当前分轮写入测试。连续发送时，单块池在交接后必须等待归还；它不是持续吞吐或端到端延时的通用最优配置。若要求逐消息立即可见，就需要更频繁发布、扫描或 flush，必须重新衡量写入成本。

当前机器无法硬绑核，存在调度变化；最接近的两个实现已经落在同一测量档位。现有证据支持“写入 p50 基本追平”，不支持“任何负载、任何运行均为最低”。

### 已确认的完整构造参数



```
{
  "transport": "two_buf_queue",
  "mode": "write_latency",
  "payload_bytes": 32,
  "messages": 10000000,
  "warmup_messages": 50000,
  "callback_iterations": 0,
  "verify_payload": false,
  "prepared_input": true,
  "variable_payload": true,
  "prepared_input_limit_bytes": 268435456,
  "measure_callback_time": true,
  "producer_interval_ns": 0,
  "source_block_size": 8388608,
  "source_pool_min": 1,
  "source_pool_max": 1,
  "source_queue_capacity": 512,
  "writer_backpressure": "drop",
  "writer_spin_count": 1000,
  "writer_wait_timeout_ms": 5000,
  "output_block_size": 8388608,
  "output_frame_capacity": 64,
  "output_pool_min": 1,
  "output_pool_max": 1,
  "output_queue_capacity": 512,
  "cancel_claim_mode": "tail",
  "output_mode": "spsc",
  "output_reader_mode": "wait_signal",
  "output_reader_count": 1,
  "output_retry_wait_us": 1000,
  "output_enable_stats": false,
  "is_scan_current": false,
  "reader_spin_count": 10000,
  "rescan_interval_ns": 1000000,
  "rescan_max_interval_ns": 30000000,
  "consumer_cpu": -1,
  "producer_cpu": -1,
  "producer_qos": "user_interactive",
  "round_storage_bytes": 6710886,
  "spsc_buffer_bytes": 8388608,
  "prefault_producer_buffers": true,
  "finish_timeout_ms": 5000,
  "work_wait_min_ms": 1,
  "finish_retry_ms": 1,
  "publish_count": 4294967295,
  "spsc_capacity": 0,
  "retry_yield_every": 1024,
  "spsc_idle_spins": 1000,
  "spsc_idle_sleep_ns": 0,
  "timer": "auto",
  "sample_every": 32,
  "max_samples": 1000000,
  "latency_batch_size": 1000,
  "timer_calibration_samples": 16384,
  "call_batch_size": 256,
  "random_seed": 44,
  "timeout_seconds": 180,
  "buffer_budget_bytes": 1073741824
}
```

## 逐次 p50

| 配置 | 两组 5 次平均 | 全部运行，ns/次 |
| --- | --- | --- |
| TwoBufQueue · 原 64 KiB 块 | 1.933 / 1.933 | 1.917, 1.917, 1.917, 1.958, 1.958, 1.958, 1.958, 1.917, 1.917, 1.917 |
| TwoBufQueue · 8 MiB 块 | 1.658 / 1.658 | 1.625, 1.792, 1.625, 1.625, 1.625, 1.625, 1.792, 1.625, 1.625, 1.625 |
| ReaderWriterQueue | 1.642 / 1.683 | 1.625, 1.708, 1.625, 1.625, 1.625, 1.625, 1.917, 1.625, 1.625, 1.625 |
| rigtorp/SPSCQueue | 3.575 / 3.550 | 3.542, 3.542, 3.583, 3.583, 3.625, 3.500, 3.583, 3.542, 3.542, 3.583 |
| atomic_queue | 8.067 / 8.175 | 7.708, 8.042, 8.667, 8.250, 7.667, 8.125, 7.708, 8.917, 8.083, 8.042 |

## 数据校验与复现

每条消息在计时前准备，32 字节全部随消息变化。每组配置先用完整 1,000 万条执行 FIFO、数量及全部字节校验，再执行正式测量；正式运行仍校验数量。发送失败为 0，TwoBufQueue 写端内部压力事件为 0。所有对照的写入次数与长度相同。

正式二进制 SHA-256：d042e98522f916c7a9f38d510d3dde48e9682f448a269f3e107e96e380506714。源代码仍含调查之前的未提交改动，文件哈希见 source-manifest.json。原有 8 / 16 / 32 / 64 B 完整报告保留旧配置的结果，本页是追加的 32 B 定位和复测。

[汇总与完整参数](performance_data/latency32_investigation_20261010/confirmation-summary.json) · [源代码与二进制哈希](performance_data/latency32_investigation_20261010/source-manifest.json) · [复测配置](performance_data/latency32_investigation_20261010/confirmation-configs.json) · [复测脚本](performance_data/latency32_investigation_20261010/run.py)

[第一组原始数据](performance_data/latency32_investigation_20261010/confirmation-configs-143853/raw.jsonl) · [第二组原始数据](performance_data/latency32_investigation_20261010/confirmation-configs-143931/raw.jsonl) · [计时组分类诊断](performance_data/latency32_investigation_20261010/phase11-configs-143756/classification-results.json) · [原完整报告](performance_8m_zero.md)

生成时间：2026-10-10T14:40:35.996138+08:00
