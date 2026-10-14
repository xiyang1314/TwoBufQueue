[English](https://xiyang1314.github.io/TwoBufQueue/docs/reports/index.html) · [中文](https://xiyang1314.github.io/TwoBufQueue/docs/reports/index.zh-CN.html)

# TwoBufQueue

生产者小缓存，批量复制，独立处理回调。 · 让生产者保持小缓存，让消费者拥有处理积压的空间。

[项目首页](https://xiyang1314.github.io/TwoBufQueue/README.zh-CN.html) · [中文介绍](https://xiyang1314.github.io/TwoBufQueue/README.zh-CN.html)

C++20 Apache-2.0 Apple M3 本机实测 包含完整参数与原始数据

[连续发送 · 每批从零积压开始

## 连续发送一批消息，对业务性能产生什么影响

正常执行业务 → 连续发送一批消息 → 立刻执行业务 · 2–16 MiB

**打开报告 →**](burst/performance_burst_from_empty.zh-CN.html) · [WriteSession

## 单块循环写入

256 MiB · 回调循环为 0 · 三轮独立运行

**打开报告 →**](comparison/performance_256m_zero_session_plateau.zh-CN.html) · [块交接时发布

## 普通 write：最大发布批次

256 MiB · 回调循环为 0 · 三轮独立运行

**打开报告 →**](comparison/performance_256m_zero_publish_max_plateau.zh-CN.html) · [每条消息发布

## 普通 write：每条发布

256 MiB · 回调循环为 0 · 三轮独立运行

**打开报告 →**](comparison/performance_256m_zero_publish1_plateau.zh-CN.html)

## 连续发送后的业务性能

![Business group-average p50 after continuous 2,4,8,16 MiB batches, each starting empty, with normal business at 13 ns per call](../assets/burst_business_cost.svg)

正常执行业务 → 连续发送一批消息 → 立刻执行相同业务。每批从零积压开始，消费者持续运行，排空不计时。所有队列使用相同的 48 KiB 业务数据、32 B 消息和 64 次回调循环。TwoBufQueue 使用 16 KiB 第一层缓存与 256 MiB 第二层缓存，开源 SPSC 使用 256 MiB 槽位存储。

正常业务约为 13 ns/次（虚线）。发送 16 MiB 后，TwoBufQueue 为 18.2 ns/次（1.40 倍），ReaderWriterQueue 为 58.2 ns，rigtorp 为 42.5 ns，atomic_queue 为 99.8 ns。发送 2 MiB 后，TwoBufQueue 与 rigtorp 均为 15.6 ns/次。

每组 16 次业务调用平均耗时的 p50，再取三轮平均。生产者采用高 QoS，未硬绑核。 [完整结果、比值与初始化参数](https://xiyang1314.github.io/TwoBufQueue/docs/reports/burst/performance_burst_from_empty.zh-CN.html).

## 指标怎么看

写入报告按每组 1,000 次发送统计生产者提交耗时；业务报告按每组 16 次调用统计业务执行耗时。它们不是单条消息或端到端延时。各报告公开线程数、两层缓存和对照负载的复制方式，结果适用于记录的测试配置。

[测试方法与复现](https://xiyang1314.github.io/TwoBufQueue/docs/reports/PERFORMANCE.zh-CN.html) · [发布内容](https://xiyang1314.github.io/TwoBufQueue/docs/development/RELEASE_CONTENTS.zh-CN.html) · [发布步骤](https://xiyang1314.github.io/TwoBufQueue/docs/development/PUBLISHING.zh-CN.html)

### 其他测量与历史实验



- [performance 256m callback](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_256m_callback.html)
- [performance 256m callback session](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_256m_callback_session.html)
- [performance 8m 16b 32b 64b tuned](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_8m_16b_32b_64b_tuned.html)
- [performance 8m 16b 32b tuned](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_8m_16b_32b_tuned.html)
- [performance 8m 32b latency](https://xiyang1314.github.io/TwoBufQueue/docs/reports/analysis/performance_8m_32b_latency.html)
- [performance 8m zero](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_8m_zero.html)
- [performance 8m zero session](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_8m_zero_session.html)
- [performance burst business sizes](https://xiyang1314.github.io/TwoBufQueue/docs/reports/burst/performance_burst_business_sizes.html)
- [performance burst tail 1m](https://xiyang1314.github.io/TwoBufQueue/docs/reports/burst/performance_burst_tail_1m.html)
- [performance burst tail instruction 1m](https://xiyang1314.github.io/TwoBufQueue/docs/reports/burst/performance_burst_tail_instruction_1m.html)
- [performance hot path cache](https://xiyang1314.github.io/TwoBufQueue/docs/reports/analysis/performance_hot_path_cache.html)
- [performance optimal ordinary readback reader2g zero](https://xiyang1314.github.io/TwoBufQueue/docs/reports/analysis/performance_optimal_ordinary_readback_reader2g_zero.html)
- [performance optimal three modes reader2g zero](https://xiyang1314.github.io/TwoBufQueue/docs/reports/analysis/performance_optimal_three_modes_reader2g_zero.html)
- [test per write block size only writer publish1](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_write_block_size_only_writer_publish1.html)
- [test per write block size only writer publish max](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_write_block_size_only_writer_publish_max.html)
- [test per write block size only writer reader80 3runs publish1](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_write_block_size_only_writer_reader80_3runs_publish1.html)
- [test per write block size only writer reader80 3runs publish max](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_write_block_size_only_writer_reader80_3runs_publish_max.html)
- [test per write block size readback fixed reader 3runs](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_write_block_size_readback_fixed_reader_3runs.html)
- [test per writer block size](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_writer_block_size.html)
- [test per writer block size cache hits](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_writer_block_size_cache_hits.html)
- [test per writer block size max comparison 3runs](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_writer_block_size_max_comparison_3runs.html)
- [test per writer block size no current](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_writer_block_size_no_current.html)
- [test per writer block size no current publish1](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_writer_block_size_no_current_publish1.html)
- [test per writer block size reader80 3runs max](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_writer_block_size_reader80_3runs_max.html)

报告保留原始测量日期；打包不会重跑或替换测试结果。
