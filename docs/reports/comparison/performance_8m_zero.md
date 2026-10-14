TWOBUFQUEUE / PERFORMANCE NOTES

# 8 MiB 缓存 · 回调循环为 0 · 普通 write

固定长度、不写长度前缀的 TwoBufQueue，与三种开源 SPSC 队列对比。TwoBufQueue 包含完整第二层复制与独立回调线程；第三方使用原生消费线程逐条回调。

10 百万条 / 次 5 次平均 / 指标 8 B / 16 B / 32 B / 64 B publish_count = 10 扫描当前块 · 不主动 flush 高 QoS · 未硬绑核

[总览](#按消息长度查看吞吐) · [吞吐与延时](#吞吐与写入耗时) · [时间与背压](#回调时间与背压) · [原始值](#逐次测量值) · [测试口径](#测试口径与复现信息)

其他报告：[256M 普通](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_256m_callback.html) · [256M Session](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_256m_callback_session.html) · [8M Session](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_8m_zero_session.html)

## 先看负载复制的敏感性

8 MiB、空回调的独立 A/B：每次 1,000 万条，5 次平均；只改外部 Payload 复制构造，完整校验通过。

这是此前两种复制方式的独立实验，未使用本次新增的写预取；不计入本轮主表运行次数；记录时间：2026-10-11T00:16:44.565737+08:00。

**本次正式表格固定采用 完整负载复制；rigtorp 可选目标写预取，ReaderWriterQueue 可选先清零再 memcpy。** 仅改变复制构造就能显著改变部分开源队列的结果。后面的差距包含负载类型与编译器代码生成的影响，不能将它全部算成队列算法的优势。具体处理器层面的原因尚未确定；默认复制构造的额外校验也未消除异常。

| 实现 / 长度 | 成员初始化 · p50 ns | 构造函数体 memcpy · p50 ns |
| --- | --- | --- |
| ReaderWriterQueue · 8 B | 1.292 | 2.192 |
| rigtorp/SPSCQueue · 8 B | 22.100 | 2.625 |
| atomic_queue · 8 B | 5.433 | 5.475 |
| ReaderWriterQueue · 16 B | 9.400 | 1.292 |
| rigtorp/SPSCQueue · 16 B | 23.475 | 2.658 |
| atomic_queue · 16 B | 6.858 | 6.842 |
| ReaderWriterQueue · 32 B | 19.633 | 1.675 |
| rigtorp/SPSCQueue · 32 B | 23.492 | 3.233 |
| atomic_queue · 32 B | 8.450 | 8.667 |
| ReaderWriterQueue · 64 B | 2.967 | 3.500 |
| rigtorp/SPSCQueue · 64 B | 20.983 | 4.317 |
| atomic_queue · 64 B | 10.667 | 10.592 |

两个版本的默认成员都有 bytes{}；构造函数体 memcpy 版本会先执行默认初始化。全部试验保留，没有按队列分别选择复制方式。[对照汇总](../../performance_data/copy_sensitivity_20261011/summary.json) · [144 次原始记录](../../performance_data/copy_sensitivity_20261011/raw.jsonl) · [对照源码](../../performance_data/copy_sensitivity_20261011/variant-source.tar.gz)

## 按消息长度查看吞吐

条形长度仅在同一消息长度内比较。单位 M条/s = 每秒百万条消息。

160 次正式测量 16 次负载校验 消息计数 / FIFO 校验通过 写入失败为 0 p999 样本量提示

TwoBufQueue 的第一层与第二层各 8 MiB，合计 16 MiB；三种开源 SPSC 请求 8 MiB。ReaderWriterQueue 会按内部块容量略微取整。

每条消息的模拟业务循环为 0；仍保留逐消息回调、序号/条数校验和队列自身的消费工作。

本次没有重新寻优，吞吐和各延时指标使用同一构造配置。普通 write 与 Session 的读端/发布配置和测量轮次可能不同，不能当作同参数对照。

只重新测量 normal 与三个开源对照；另一写模式的报告保持原样，没有把它的历史数据混入本轮主表。

每层数据池严格为 8 MiB；原生队列的保留槽与 rigtorp 首尾 padding 纳入这个 payload 存储预算，队列元数据和输入另计。

Session 正常遇到块满返回 insufficient_space，次数记入 session_block_exhaustions；这不是背压或丢消息。池/队列受压通过 Writer 内部压力统计报告。

rigtorp_write_prefetch 在完整参数中记录；开关在测试开始前选择模板，普通复制和带提示构造都只复制一次完整负载，不额外写零。atomic_queue 保持原有调用。

ReaderWriterQueue 16/32 B 使用 copy_after_zero：队列槽位默认清零后 memcpy 一次完整负载；8/64 B 保留成员复制。清零和复制均在写入计时内。各实现的调用端优化在行标签和完整参数中公开，不能把所有差异归因于队列算法。

TwoBufQueue 8 B：is_scan_current=true, publish_count=10, 初始/最大扫描间隔=1000000/1000000 ns，最小等待=1 ms。

TwoBufQueue 16 B：is_scan_current=true, publish_count=10, 初始/最大扫描间隔=1000000/1000000 ns，最小等待=1 ms。

TwoBufQueue 32 B：is_scan_current=true, publish_count=10, 初始/最大扫描间隔=1000000/1000000 ns，最小等待=1 ms。

TwoBufQueue 64 B：is_scan_current=true, publish_count=10, 初始/最大扫描间隔=1000000/1000000 ns，最小等待=1 ms。

本轮只改变普通 write 的当前块扫描和发布参数；Session 的配置、报告和原始结果保持上一轮原样。三个开源对照本轮重新测量，未使用历史成绩替换。

256 MiB 的 1/1/2/4 次回调循环沿用 release_20261011_inline_fixed_110024；8 MiB 仍为 0。本次合计 320 次正式测量、32 次独立完整负载/FIFO 校验，没有重新校准或寻优。

测试框架增加 publish_count=10 的编译期 Writer 实例，并修正非 2 次幂发布次数的轮末自然发布对齐；未修改库 src/include 或第三方头文件。每轮少于 1000 条的尾部仍发送并记录，延时样本只统计完整的 1000 次写入组。

消息长度

8 B 吞吐 · M条/s

TwoBufQueue 1,235.94

ReaderWriterQueue 654.07

rigtorp/SPSCQueue 339.42

atomic_queue 187.76

16 B 吞吐 · M条/s

TwoBufQueue 1,041.12

ReaderWriterQueue 730.23

rigtorp/SPSCQueue 321.57

atomic_queue 128.79

32 B 吞吐 · M条/s

TwoBufQueue 143.26

ReaderWriterQueue 494.79

rigtorp/SPSCQueue 289.65

atomic_queue 84.87

64 B 吞吐 · M条/s

TwoBufQueue 125.27

ReaderWriterQueue 312.81

rigtorp/SPSCQueue 166.05

atomic_queue 69.68

## 吞吐与写入耗时

每个指标先按单次运行计算，再取 5 次算术平均。TwoBufQueue 整行加粗并加边框。

**8 B 回调循环 0 次 · 每次 12 轮 · 10,000,000 条**

| 实现 | 吞吐 · M条/s · 下方为范围 | 有效负载 · GB/s | p50 · ns / 次 | p99 · ns / 次 | p999 · ns / 次 | 回调区间 / 写入 · 墙钟时间比 | 回调循环 · 次 / 消息 | 分轮 · 轮 / 运行 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 普通 write · 固定长度写** | **1,235.94 · 1,226.95 – 1,252.80** | **9.888** | **0.758** | **1.425** | **2.650** | **3.49×** | **0** | **12** |
| ReaderWriterQueue · 成员初始化复制 | 654.07 · 519.56 – 713.38 | 5.233 | 1.250 | 1.883 | 4.608 | 2.55× | 0 | 12 |
| rigtorp/SPSCQueue · 成员复制＋目标写预取 | 339.42 · 328.64 – 344.62 | 2.715 | 2.792 | 4.350 | 8.242 | 1.00× | 0 | 12 |
| atomic_queue · 成员初始化复制 | 187.76 · 179.88 – 202.03 | 1.502 | 4.942 | 10.383 | 26.108 | 1.15× | 0 | 12 |

**16 B 回调循环 0 次 · 每次 24 轮 · 10,000,000 条**

| 实现 | 吞吐 · M条/s · 下方为范围 | 有效负载 · GB/s | p50 · ns / 次 | p99 · ns / 次 | p999 · ns / 次 | 回调区间 / 写入 · 墙钟时间比 | 回调循环 · 次 / 消息 | 分轮 · 轮 / 运行 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 普通 write · 固定长度写** | **1,041.12 · 1,001.79 – 1,096.29** | **16.658** | **0.833** | **1.908** | **4.258** | **4.21×** | **0** | **24** |
| ReaderWriterQueue · 先清零，再 memcpy | 730.23 · 699.79 – 742.81 | 11.684 | 1.292 | 1.967 | 3.633 | 3.05× | 0 | 24 |
| rigtorp/SPSCQueue · 成员复制＋目标写预取 | 321.57 · 314.35 – 326.85 | 5.145 | 2.983 | 4.850 | 9.075 | 1.00× | 0 | 24 |
| atomic_queue · 成员初始化复制 | 128.79 · 125.53 – 132.46 | 2.061 | 6.058 | 24.392 | 35.175 | 1.01× | 0 | 24 |

**32 B 回调循环 0 次 · 每次 48 轮 · 10,000,000 条**

| 实现 | 吞吐 · M条/s · 下方为范围 | 有效负载 · GB/s | p50 · ns / 次 | p99 · ns / 次 | p999 · ns / 次 | 回调区间 / 写入 · 墙钟时间比 | 回调循环 · 次 / 消息 | 分轮 · 轮 / 运行 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 普通 write · 固定长度写** | **143.26 · 142.64 – 144.28** | **4.584** | **6.517** | **16.550** | **25.917** | **1.34×** | **0** | **48** |
| ReaderWriterQueue · 先清零，再 memcpy | 494.79 · 456.04 – 535.28 | 15.833 | 1.683 | 10.408 | 12.175 | 2.14× | 0 | 48 |
| rigtorp/SPSCQueue · 成员复制＋目标写预取 | 289.65 · 278.37 – 295.97 | 9.269 | 3.092 | 7.025 | 10.067 | 1.00× | 0 | 48 |
| atomic_queue · 成员初始化复制 | 84.87 · 80.96 – 88.52 | 2.716 | 8.583 | 25.058 | 36.233 | 1.00× | 0 | 48 |

**64 B 回调循环 0 次 · 每次 96 轮 · 10,000,000 条**

| 实现 | 吞吐 · M条/s · 下方为范围 | 有效负载 · GB/s | p50 · ns / 次 | p99 · ns / 次 | p999 · ns / 次 | 回调区间 / 写入 · 墙钟时间比 | 回调循环 · 次 / 消息 | 分轮 · 轮 / 运行 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 普通 write · 固定长度写** | **125.27 · 123.28 – 126.32** | **8.017** | **7.308** | **18.700** | **27.267** | **1.74×** | **0** | **96** |
| ReaderWriterQueue · 成员初始化复制 | 312.81 · 302.68 – 323.83 | 20.020 | 3.292 | 8.217 | 13.408 | 1.35× | 0 | 96 |
| rigtorp/SPSCQueue · 成员复制＋目标写预取 | 166.05 · 143.29 – 180.75 | 10.627 | 6.217 | 12.617 | 22.183 | 1.00× | 0 | 96 |
| atomic_queue · 成员初始化复制 | 69.68 · 65.61 – 72.36 | 4.459 | 13.333 | 30.133 | 38.908 | 1.02× | 0 | 96 |

p50 / p99 / p999 是“每组 1000 次成功写入的平均耗时”的分位数，不是单条消息端到端延时，也不是单次 write 的分位数。吞吐使用独立的未逐组计时运行。GB/s 按业务净负载计算。时间比 = 各轮首条回调开始至末条回调结束的耗时之和 ÷ 各轮写线程发送耗时之和，再对运行取平均；它包含分发、等待数据和调度，不是每次回调函数 CPU 耗时之和。

## 回调时间与背压

与吞吐表使用同一批正式运行。时间、次数按运行取平均；峰值 RSS 为这组运行的最大值。

**8 B 正式吞吐运行的逐次平均；RSS 为本组最大值**

| 实现 | 写线程 · ms | 回调区间 · ms | 受压消息 · 条 / 次运行 | 失败尝试 · 次 / 运行 | 重试时间 · ms | Writer 压力 · 内部事件 | 复制层拒绝 · 内部事件 | 峰值 RSS · MiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue** | **8.09** | **28.24** | **0.0** | **0.0** | **0.000** | **0** | **1** | **24.4** |
| ReaderWriterQueue | 15.49 | 38.94 | 0.0 | 0.0 | 0.000 | — | — | 17.1 |
| rigtorp/SPSCQueue | 29.47 | 29.50 | 0.0 | 0.0 | 0.000 | — | — | 16.1 |
| atomic_queue | 53.35 | 61.10 | 0.0 | 0.0 | 0.000 | — | — | 17.1 |

**16 B 正式吞吐运行的逐次平均；RSS 为本组最大值**

| 实现 | 写线程 · ms | 回调区间 · ms | 受压消息 · 条 / 次运行 | 失败尝试 · 次 / 运行 | 重试时间 · ms | Writer 压力 · 内部事件 | 复制层拒绝 · 内部事件 | 峰值 RSS · MiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue** | **9.61** | **40.42** | **0.0** | **0.0** | **0.000** | **0** | **8** | **30.6** |
| ReaderWriterQueue | 13.70 | 41.81 | 0.0 | 0.0 | 0.000 | — | — | 16.7 |
| rigtorp/SPSCQueue | 31.10 | 31.17 | 0.0 | 0.0 | 0.000 | — | — | 16.1 |
| atomic_queue | 77.68 | 78.58 | 0.0 | 0.0 | 0.000 | — | — | 16.6 |

**32 B 正式吞吐运行的逐次平均；RSS 为本组最大值**

| 实现 | 写线程 · ms | 回调区间 · ms | 受压消息 · 条 / 次运行 | 失败尝试 · 次 / 运行 | 重试时间 · ms | Writer 压力 · 内部事件 | 复制层拒绝 · 内部事件 | 峰值 RSS · MiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue** | **69.80** | **93.24** | **0.0** | **0.0** | **0.000** | **0** | **7** | **24.2** |
| ReaderWriterQueue | 20.27 | 43.28 | 0.0 | 0.0 | 0.000 | — | — | 16.5 |
| rigtorp/SPSCQueue | 34.54 | 34.59 | 0.0 | 0.0 | 0.000 | — | — | 16.2 |
| atomic_queue | 117.94 | 118.31 | 0.0 | 0.0 | 0.000 | — | — | 16.5 |

**64 B 正式吞吐运行的逐次平均；RSS 为本组最大值**

| 实现 | 写线程 · ms | 回调区间 · ms | 受压消息 · 条 / 次运行 | 失败尝试 · 次 / 运行 | 重试时间 · ms | Writer 压力 · 内部事件 | 复制层拒绝 · 内部事件 | 峰值 RSS · MiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue** | **79.84** | **138.76** | **0.0** | **0.0** | **0.000** | **0** | **0** | **24.2** |
| ReaderWriterQueue | 31.98 | 43.11 | 0.0 | 0.0 | 0.000 | — | — | 20.2 |
| rigtorp/SPSCQueue | 60.60 | 60.63 | 0.0 | 0.0 | 0.000 | — | — | 16.2 |
| atomic_queue | 143.68 | 145.96 | 0.0 | 0.0 | 0.000 | — | — | 16.2 |

受压消息至少经历一次 try 写入失败；失败尝试含重复重试，最终仍全部写入。Writer 与复制层内部事件仅 TwoBufQueue 提供，其他实现显示“—”，不能把“—”解释为 0。回调区间含分发、轮内等待及调度，时间比大于 1 本身不等于写端背压。RSS 包含进程开销，不等于缓存容量。

## 逐次测量值

展开查看全部 5 次吞吐及分位数。没有剔除异常值，也没有将多个运行的样本合并计算分位数。

### 8 B 5 次原始值 · 回调循环 0 次



| 实现 / 指标 | 第 1 次 | 第 2 次 | 第 3 次 | 第 4 次 | 第 5 次 | 平均 |
| --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 吞吐 M条/s** | **1,227.929** | **1,226.950** | **1,252.799** | **1,235.286** | **1,236.757** | **1,235.944** |
| **TwoBufQueue · p50 ns** | **0.750** | **0.750** | **0.792** | **0.750** | **0.750** | **0.758** |
| **TwoBufQueue · p99 ns** | **1.125** | **1.583** | **1.375** | **1.167** | **1.875** | **1.425** |
| **TwoBufQueue · p999 ns** | **2.208** | **2.583** | **2.792** | **2.667** | **3.000** | **2.650** |
| ReaderWriterQueue · 吞吐 M条/s | 519.556 | 665.172 | 713.381 | 677.113 | 695.108 | 654.066 |
| ReaderWriterQueue · p50 ns | 1.250 | 1.250 | 1.250 | 1.250 | 1.250 | 1.250 |
| ReaderWriterQueue · p99 ns | 2.042 | 1.958 | 1.792 | 1.875 | 1.750 | 1.883 |
| ReaderWriterQueue · p999 ns | 5.750 | 5.083 | 3.833 | 4.000 | 4.375 | 4.608 |
| rigtorp/SPSCQueue · 吞吐 M条/s | 336.377 | 328.638 | 344.625 | 343.963 | 343.507 | 339.422 |
| rigtorp/SPSCQueue · p50 ns | 2.792 | 2.792 | 2.792 | 2.792 | 2.792 | 2.792 |
| rigtorp/SPSCQueue · p99 ns | 4.375 | 4.625 | 4.167 | 4.333 | 4.250 | 4.350 |
| rigtorp/SPSCQueue · p999 ns | 5.875 | 11.000 | 6.792 | 10.083 | 7.458 | 8.242 |
| atomic_queue · 吞吐 M条/s | 179.884 | 182.657 | 189.661 | 184.585 | 202.028 | 187.763 |
| atomic_queue · p50 ns | 4.917 | 4.875 | 4.833 | 5.167 | 4.917 | 4.942 |
| atomic_queue · p99 ns | 10.542 | 11.917 | 9.917 | 9.208 | 10.333 | 10.383 |
| atomic_queue · p999 ns | 26.167 | 31.167 | 25.833 | 22.208 | 25.167 | 26.108 |

### 16 B 5 次原始值 · 回调循环 0 次



| 实现 / 指标 | 第 1 次 | 第 2 次 | 第 3 次 | 第 4 次 | 第 5 次 | 平均 |
| --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 吞吐 M条/s** | **1,023.865** | **1,027.485** | **1,096.291** | **1,001.795** | **1,056.189** | **1,041.125** |
| **TwoBufQueue · p50 ns** | **0.792** | **0.833** | **0.875** | **0.833** | **0.833** | **0.833** |
| **TwoBufQueue · p99 ns** | **1.917** | **2.125** | **2.042** | **1.875** | **1.583** | **1.908** |
| **TwoBufQueue · p999 ns** | **2.875** | **8.833** | **3.958** | **2.833** | **2.792** | **4.258** |
| ReaderWriterQueue · 吞吐 M条/s | 742.813 | 739.476 | 736.797 | 699.788 | 732.274 | 730.230 |
| ReaderWriterQueue · p50 ns | 1.292 | 1.292 | 1.292 | 1.292 | 1.292 | 1.292 |
| ReaderWriterQueue · p99 ns | 2.000 | 2.000 | 1.917 | 1.875 | 2.042 | 1.967 |
| ReaderWriterQueue · p999 ns | 2.958 | 4.458 | 3.917 | 3.042 | 3.792 | 3.633 |
| rigtorp/SPSCQueue · 吞吐 M条/s | 323.237 | 314.346 | 320.422 | 322.974 | 326.849 | 321.566 |
| rigtorp/SPSCQueue · p50 ns | 3.000 | 2.958 | 2.958 | 3.000 | 3.000 | 2.983 |
| rigtorp/SPSCQueue · p99 ns | 5.458 | 4.792 | 4.833 | 4.500 | 4.667 | 4.850 |
| rigtorp/SPSCQueue · p999 ns | 13.583 | 9.583 | 7.458 | 7.500 | 7.250 | 9.075 |
| atomic_queue · 吞吐 M条/s | 126.493 | 125.529 | 132.462 | 130.235 | 129.207 | 128.785 |
| atomic_queue · p50 ns | 6.083 | 5.958 | 5.958 | 6.125 | 6.167 | 6.058 |
| atomic_queue · p99 ns | 24.250 | 24.458 | 24.458 | 23.792 | 25.000 | 24.392 |
| atomic_queue · p999 ns | 30.958 | 32.417 | 34.042 | 33.583 | 44.875 | 35.175 |

### 32 B 5 次原始值 · 回调循环 0 次



| 实现 / 指标 | 第 1 次 | 第 2 次 | 第 3 次 | 第 4 次 | 第 5 次 | 平均 |
| --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 吞吐 M条/s** | **142.835** | **142.799** | **142.637** | **143.764** | **144.280** | **143.263** |
| **TwoBufQueue · p50 ns** | **6.542** | **6.542** | **6.500** | **6.542** | **6.458** | **6.517** |
| **TwoBufQueue · p99 ns** | **16.750** | **16.542** | **17.000** | **15.625** | **16.833** | **16.550** |
| **TwoBufQueue · p999 ns** | **25.708** | **26.208** | **26.333** | **25.208** | **26.125** | **25.917** |
| ReaderWriterQueue · 吞吐 M条/s | 535.280 | 456.036 | 487.023 | 486.281 | 509.323 | 494.788 |
| ReaderWriterQueue · p50 ns | 1.625 | 1.667 | 1.792 | 1.667 | 1.667 | 1.683 |
| ReaderWriterQueue · p99 ns | 10.292 | 10.167 | 10.792 | 10.500 | 10.292 | 10.408 |
| ReaderWriterQueue · p999 ns | 10.917 | 10.583 | 18.000 | 10.875 | 10.500 | 12.175 |
| rigtorp/SPSCQueue · 吞吐 M条/s | 295.969 | 278.372 | 293.338 | 286.395 | 294.197 | 289.654 |
| rigtorp/SPSCQueue · p50 ns | 3.083 | 3.125 | 3.083 | 3.083 | 3.083 | 3.092 |
| rigtorp/SPSCQueue · p99 ns | 6.458 | 7.958 | 6.250 | 6.083 | 8.375 | 7.025 |
| rigtorp/SPSCQueue · p999 ns | 10.125 | 10.917 | 9.042 | 9.375 | 10.875 | 10.067 |
| atomic_queue · 吞吐 M条/s | 80.962 | 83.389 | 85.288 | 86.178 | 88.524 | 84.868 |
| atomic_queue · p50 ns | 8.542 | 8.875 | 8.583 | 8.375 | 8.542 | 8.583 |
| atomic_queue · p99 ns | 25.667 | 24.917 | 24.125 | 25.792 | 24.792 | 25.058 |
| atomic_queue · p999 ns | 33.375 | 45.208 | 33.375 | 37.208 | 32.000 | 36.233 |

### 64 B 5 次原始值 · 回调循环 0 次



| 实现 / 指标 | 第 1 次 | 第 2 次 | 第 3 次 | 第 4 次 | 第 5 次 | 平均 |
| --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 吞吐 M条/s** | **126.320** | **125.680** | **123.275** | **125.385** | **125.668** | **125.266** |
| **TwoBufQueue · p50 ns** | **7.333** | **7.333** | **7.292** | **7.292** | **7.292** | **7.308** |
| **TwoBufQueue · p99 ns** | **19.042** | **18.125** | **18.667** | **18.042** | **19.625** | **18.700** |
| **TwoBufQueue · p999 ns** | **31.333** | **25.042** | **26.583** | **26.000** | **27.375** | **27.267** |
| ReaderWriterQueue · 吞吐 M条/s | 323.835 | 302.681 | 311.353 | 309.889 | 316.278 | 312.807 |
| ReaderWriterQueue · p50 ns | 3.708 | 3.458 | 3.000 | 3.292 | 3.000 | 3.292 |
| ReaderWriterQueue · p99 ns | 6.792 | 9.958 | 7.083 | 10.292 | 6.958 | 8.217 |
| ReaderWriterQueue · p999 ns | 10.750 | 19.333 | 9.917 | 16.250 | 10.792 | 13.408 |
| rigtorp/SPSCQueue · 吞吐 M条/s | 180.752 | 167.858 | 143.292 | 173.546 | 164.823 | 166.054 |
| rigtorp/SPSCQueue · p50 ns | 5.375 | 8.958 | 5.417 | 5.625 | 5.708 | 6.217 |
| rigtorp/SPSCQueue · p99 ns | 11.000 | 22.250 | 9.708 | 9.958 | 10.167 | 12.617 |
| rigtorp/SPSCQueue · p999 ns | 18.750 | 47.917 | 13.958 | 14.292 | 16.000 | 22.183 |
| atomic_queue · 吞吐 M条/s | 69.806 | 72.357 | 71.395 | 65.613 | 69.209 | 69.676 |
| atomic_queue · p50 ns | 14.583 | 13.083 | 13.167 | 12.125 | 13.708 | 13.333 |
| atomic_queue · p99 ns | 30.292 | 30.417 | 30.000 | 29.792 | 30.167 | 30.133 |
| atomic_queue · p999 ns | 43.667 | 33.917 | 40.958 | 39.292 | 36.708 | 38.908 |

波动较大的吞吐组：8 B · ReaderWriterQueue：519.56–713.38 M条/s；64 B · rigtorp/SPSCQueue：143.29–180.75 M条/s。所有测量均保留，未删除低值。

## 测试口径与复现信息

固定本次参数进行对比；本报告没有执行全参数寻优，也不声称全局最优。

### 发送、复制与排空

TwoBufQueue：Writer → 复制线程 → 第二层缓存 → 独立回调线程，逐消息调用回调。

按长度保留冻结的块大小和每层容量。普通 write 与 Session 的读端及发布配置各自独立，完整参数如下。四种实现正式写入次数与净负载长度相同。

每轮上限按缓存的 80% / 消息字节数计算，再停在实际发布批次的整数倍位置。TwoBufQueue 开启当前块扫描，无逐条或轮末主动 flush。停止写线程计时后等待本轮全部回调完成；最终尾部由正常 finish 收尾。

两份报告同时改变缓存大小与回调工作量，不能将两组差异全部归因于缓存；比较容量影响需另做相同回调负载的对照。

扫描当前块 · 不主动 flush；publish_count 固定为 10。仅预触碰写端数据页，第二层数据页没有额外预触碰。

吞吐和分组延时均在计时前准备整轮输入；每条消息的全部字都随序号变化；四种实现使用同一完整负载；队列内仍复制一次完整负载；各行标明成员复制、目标写预取或先清零再 memcpy，完整参数记录实际选择；先清零的额外存储也包含在计时中。输入缓冲单独计算，不算队列容量，也不保证常驻 L1。

### 时钟、调度与限制

本机为 macOS-14.4.1-arm64-arm-64bit / arm64，Release 优化构建。

计时器：mach_absolute_time；每组 1000 次写入。组均值刻度约 0.0417 ns / 次。

写线程调度：高 QoS · 未硬绑核。调度打断和其他系统活动仍可能影响结果；复制与回调线程保留自身调度策略。

吞吐只累计写线程发送时间；排空在计时之外。并发复制 / 回调仍可能争用缓存和带宽，因此不是持续端到端吞吐。

**p999 仅供观察：** 每次延时测量约有 9,918–9,994 个组均值样本，少于框架建议的 100,000 个。p999 的尾部覆盖有限，不能据此做稳定的尾延时承诺。p999 表示 99.9% 分位数；平均值不代表置信区间。

### 完整构造参数 · 每种长度 / 实现 / 优化指标



```
{
  "8 B / two_buf_queue": {
    "transport": "two_buf_queue",
    "mode": "throughput",
    "payload_bytes": 8,
    "messages": 10000000,
    "warmup_messages": 50000,
    "callback_iterations": 0,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 1048576,
    "source_pool_min": 8,
    "source_pool_max": 8,
    "source_queue_capacity": 32,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 1048576,
    "output_frame_capacity": 1024,
    "output_pool_min": 8,
    "output_pool_max": 8,
    "output_queue_capacity": 32,
    "cancel_claim_mode": "tail",
    "output_mode": "spsc",
    "output_reader_mode": "wait_signal",
    "output_reader_count": 1,
    "output_retry_wait_us": 1000,
    "output_enable_stats": false,
    "is_scan_current": true,
    "reader_spin_count": 1000,
    "rescan_interval_ns": 1000000,
    "rescan_max_interval_ns": 1000000,
    "consumer_cpu": -1,
    "producer_cpu": -1,
    "producer_qos": "user_interactive",
    "round_storage_bytes": 6710886,
    "spsc_buffer_bytes": 8388608,
    "prefault_producer_buffers": true,
    "finish_timeout_ms": 5000,
    "work_wait_min_ms": 1,
    "finish_retry_ms": 1,
    "publish_count": 10,
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
    "random_seed": 42,
    "timeout_seconds": 180,
    "buffer_budget_bytes": 16777216,
    "use_write_session": false,
    "rigtorp_write_prefetch": false,
    "readerwriterqueue_copy_after_zero": false
  },
  "8 B / readerwriterqueue": {
    "transport": "readerwriterqueue",
    "mode": "throughput",
    "payload_bytes": 8,
    "messages": 10000000,
    "warmup_messages": 50000,
    "callback_iterations": 0,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 1048576,
    "source_pool_min": 8,
    "source_pool_max": 8,
    "source_queue_capacity": 32,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 1048576,
    "output_frame_capacity": 1024,
    "output_pool_min": 8,
    "output_pool_max": 8,
    "output_queue_capacity": 32,
    "cancel_claim_mode": "tail",
    "output_mode": "spsc",
    "output_reader_mode": "wait_signal",
    "output_reader_count": 1,
    "output_retry_wait_us": 1000,
    "output_enable_stats": false,
    "is_scan_current": false,
    "reader_spin_count": 1000,
    "rescan_interval_ns": 5000000,
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
    "spsc_capacity": 1046017,
    "retry_yield_every": 1024,
    "spsc_idle_spins": 1000,
    "spsc_idle_sleep_ns": 10000,
    "timer": "auto",
    "sample_every": 32,
    "max_samples": 1000000,
    "latency_batch_size": 1000,
    "timer_calibration_samples": 16384,
    "call_batch_size": 256,
    "random_seed": 42,
    "timeout_seconds": 180,
    "buffer_budget_bytes": 16777216,
    "use_write_session": false,
    "rigtorp_write_prefetch": false,
    "readerwriterqueue_copy_after_zero": false
  },
  "8 B / rigtorp": {
    "transport": "rigtorp",
    "mode": "throughput",
    "payload_bytes": 8,
    "messages": 10000000,
    "warmup_messages": 50000,
    "callback_iterations": 0,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 1048576,
    "source_pool_min": 8,
    "source_pool_max": 8,
    "source_queue_capacity": 32,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 1048576,
    "output_frame_capacity": 1024,
    "output_pool_min": 8,
    "output_pool_max": 8,
    "output_queue_capacity": 32,
    "cancel_claim_mode": "tail",
    "output_mode": "spsc",
    "output_reader_mode": "wait_signal",
    "output_reader_count": 1,
    "output_retry_wait_us": 1000,
    "output_enable_stats": false,
    "is_scan_current": false,
    "reader_spin_count": 1000,
    "rescan_interval_ns": 5000000,
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
    "spsc_capacity": 1048559,
    "retry_yield_every": 1024,
    "spsc_idle_spins": 0,
    "spsc_idle_sleep_ns": 1000,
    "timer": "auto",
    "sample_every": 32,
    "max_samples": 1000000,
    "latency_batch_size": 1000,
    "timer_calibration_samples": 16384,
    "call_batch_size": 256,
    "random_seed": 42,
    "timeout_seconds": 180,
    "buffer_budget_bytes": 16777216,
    "use_write_session": false,
    "rigtorp_write_prefetch": true,
    "readerwriterqueue_copy_after_zero": false
  },
  "8 B / atomic_queue": {
    "transport": "atomic_queue",
    "mode": "throughput",
    "payload_bytes": 8,
    "messages": 10000000,
    "warmup_messages": 50000,
    "callback_iterations": 0,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 1048576,
    "source_pool_min": 8,
    "source_pool_max": 8,
    "source_queue_capacity": 32,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 1048576,
    "output_frame_capacity": 1024,
    "output_pool_min": 8,
    "output_pool_max": 8,
    "output_queue_capacity": 32,
    "cancel_claim_mode": "tail",
    "output_mode": "spsc",
    "output_reader_mode": "wait_signal",
    "output_reader_count": 1,
    "output_retry_wait_us": 1000,
    "output_enable_stats": false,
    "is_scan_current": false,
    "reader_spin_count": 1000,
    "rescan_interval_ns": 5000000,
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
    "spsc_capacity": 1048576,
    "retry_yield_every": 1024,
    "spsc_idle_spins": 0,
    "spsc_idle_sleep_ns": 10000,
    "timer": "auto",
    "sample_every": 32,
    "max_samples": 1000000,
    "latency_batch_size": 1000,
    "timer_calibration_samples": 16384,
    "call_batch_size": 256,
    "random_seed": 42,
    "timeout_seconds": 180,
    "buffer_budget_bytes": 16777216,
    "use_write_session": false,
    "rigtorp_write_prefetch": false,
    "readerwriterqueue_copy_after_zero": false
  },
  "16 B / two_buf_queue": {
    "transport": "two_buf_queue",
    "mode": "throughput",
    "payload_bytes": 16,
    "messages": 10000000,
    "warmup_messages": 50000,
    "callback_iterations": 0,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 4194304,
    "source_pool_min": 2,
    "source_pool_max": 2,
    "source_queue_capacity": 8,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 4194304,
    "output_frame_capacity": 64,
    "output_pool_min": 2,
    "output_pool_max": 2,
    "output_queue_capacity": 8,
    "cancel_claim_mode": "tail",
    "output_mode": "spsc",
    "output_reader_mode": "wait_signal",
    "output_reader_count": 1,
    "output_retry_wait_us": 1000,
    "output_enable_stats": false,
    "is_scan_current": true,
    "reader_spin_count": 0,
    "rescan_interval_ns": 1000000,
    "rescan_max_interval_ns": 1000000,
    "consumer_cpu": -1,
    "producer_cpu": -1,
    "producer_qos": "user_interactive",
    "round_storage_bytes": 6710886,
    "spsc_buffer_bytes": 8388608,
    "prefault_producer_buffers": true,
    "finish_timeout_ms": 5000,
    "work_wait_min_ms": 1,
    "finish_retry_ms": 1,
    "publish_count": 10,
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
    "random_seed": 42,
    "timeout_seconds": 180,
    "buffer_budget_bytes": 16777216,
    "use_write_session": false,
    "rigtorp_write_prefetch": false,
    "readerwriterqueue_copy_after_zero": false
  },
  "16 B / readerwriterqueue": {
    "transport": "readerwriterqueue",
    "mode": "throughput",
    "payload_bytes": 16,
    "messages": 10000000,
    "warmup_messages": 50000,
    "callback_iterations": 0,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 1048576,
    "source_pool_min": 8,
    "source_pool_max": 8,
    "source_queue_capacity": 32,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 1048576,
    "output_frame_capacity": 1024,
    "output_pool_min": 8,
    "output_pool_max": 8,
    "output_queue_capacity": 32,
    "cancel_claim_mode": "tail",
    "output_mode": "spsc",
    "output_reader_mode": "wait_signal",
    "output_reader_count": 1,
    "output_retry_wait_us": 1000,
    "output_enable_stats": false,
    "is_scan_current": false,
    "reader_spin_count": 1000,
    "rescan_interval_ns": 5000000,
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
    "spsc_capacity": 522753,
    "retry_yield_every": 1024,
    "spsc_idle_spins": 0,
    "spsc_idle_sleep_ns": 10000,
    "timer": "auto",
    "sample_every": 32,
    "max_samples": 1000000,
    "latency_batch_size": 1000,
    "timer_calibration_samples": 16384,
    "call_batch_size": 256,
    "random_seed": 42,
    "timeout_seconds": 180,
    "buffer_budget_bytes": 16777216,
    "use_write_session": false,
    "rigtorp_write_prefetch": false,
    "readerwriterqueue_copy_after_zero": true
  },
  "16 B / rigtorp": {
    "transport": "rigtorp",
    "mode": "throughput",
    "payload_bytes": 16,
    "messages": 10000000,
    "warmup_messages": 50000,
    "callback_iterations": 0,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 1048576,
    "source_pool_min": 8,
    "source_pool_max": 8,
    "source_queue_capacity": 32,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 1048576,
    "output_frame_capacity": 1024,
    "output_pool_min": 8,
    "output_pool_max": 8,
    "output_queue_capacity": 32,
    "cancel_claim_mode": "tail",
    "output_mode": "spsc",
    "output_reader_mode": "wait_signal",
    "output_reader_count": 1,
    "output_retry_wait_us": 1000,
    "output_enable_stats": false,
    "is_scan_current": false,
    "reader_spin_count": 1000,
    "rescan_interval_ns": 5000000,
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
    "spsc_capacity": 524279,
    "retry_yield_every": 1024,
    "spsc_idle_spins": 0,
    "spsc_idle_sleep_ns": 1000,
    "timer": "auto",
    "sample_every": 32,
    "max_samples": 1000000,
    "latency_batch_size": 1000,
    "timer_calibration_samples": 16384,
    "call_batch_size": 256,
    "random_seed": 42,
    "timeout_seconds": 180,
    "buffer_budget_bytes": 16777216,
    "use_write_session": false,
    "rigtorp_write_prefetch": true,
    "readerwriterqueue_copy_after_zero": false
  },
  "16 B / atomic_queue": {
    "transport": "atomic_queue",
    "mode": "throughput",
    "payload_bytes": 16,
    "messages": 10000000,
    "warmup_messages": 50000,
    "callback_iterations": 0,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 1048576,
    "source_pool_min": 8,
    "source_pool_max": 8,
    "source_queue_capacity": 32,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 1048576,
    "output_frame_capacity": 1024,
    "output_pool_min": 8,
    "output_pool_max": 8,
    "output_queue_capacity": 32,
    "cancel_claim_mode": "tail",
    "output_mode": "spsc",
    "output_reader_mode": "wait_signal",
    "output_reader_count": 1,
    "output_retry_wait_us": 1000,
    "output_enable_stats": false,
    "is_scan_current": false,
    "reader_spin_count": 1000,
    "rescan_interval_ns": 5000000,
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
    "spsc_capacity": 524288,
    "retry_yield_every": 1024,
    "spsc_idle_spins": 0,
    "spsc_idle_sleep_ns": 10000,
    "timer": "auto",
    "sample_every": 32,
    "max_samples": 1000000,
    "latency_batch_size": 1000,
    "timer_calibration_samples": 16384,
    "call_batch_size": 256,
    "random_seed": 42,
    "timeout_seconds": 180,
    "buffer_budget_bytes": 16777216,
    "use_write_session": false,
    "rigtorp_write_prefetch": false,
    "readerwriterqueue_copy_after_zero": false
  },
  "32 B / two_buf_queue": {
    "transport": "two_buf_queue",
    "mode": "throughput",
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
    "source_block_size": 4194304,
    "source_pool_min": 2,
    "source_pool_max": 2,
    "source_queue_capacity": 8,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 4194304,
    "output_frame_capacity": 1,
    "output_pool_min": 2,
    "output_pool_max": 2,
    "output_queue_capacity": 8,
    "cancel_claim_mode": "tail",
    "output_mode": "spsc",
    "output_reader_mode": "wait_signal",
    "output_reader_count": 1,
    "output_retry_wait_us": 1000,
    "output_enable_stats": false,
    "is_scan_current": true,
    "reader_spin_count": 0,
    "rescan_interval_ns": 1000000,
    "rescan_max_interval_ns": 1000000,
    "consumer_cpu": -1,
    "producer_cpu": -1,
    "producer_qos": "user_interactive",
    "round_storage_bytes": 6710886,
    "spsc_buffer_bytes": 8388608,
    "prefault_producer_buffers": true,
    "finish_timeout_ms": 5000,
    "work_wait_min_ms": 1,
    "finish_retry_ms": 1,
    "publish_count": 10,
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
    "random_seed": 42,
    "timeout_seconds": 180,
    "buffer_budget_bytes": 16777216,
    "use_write_session": false,
    "rigtorp_write_prefetch": false,
    "readerwriterqueue_copy_after_zero": false
  },
  "32 B / readerwriterqueue": {
    "transport": "readerwriterqueue",
    "mode": "throughput",
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
    "source_block_size": 65536,
    "source_pool_min": 128,
    "source_pool_max": 128,
    "source_queue_capacity": 512,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 65536,
    "output_frame_capacity": 1024,
    "output_pool_min": 128,
    "output_pool_max": 128,
    "output_queue_capacity": 512,
    "cancel_claim_mode": "tail",
    "output_mode": "spsc",
    "output_reader_mode": "wait_signal",
    "output_reader_count": 1,
    "output_retry_wait_us": 1000,
    "output_enable_stats": false,
    "is_scan_current": false,
    "reader_spin_count": 10000,
    "rescan_interval_ns": 5000000,
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
    "spsc_capacity": 261121,
    "retry_yield_every": 1024,
    "spsc_idle_spins": 0,
    "spsc_idle_sleep_ns": 1000,
    "timer": "auto",
    "sample_every": 32,
    "max_samples": 1000000,
    "latency_batch_size": 1000,
    "timer_calibration_samples": 16384,
    "call_batch_size": 256,
    "random_seed": 42,
    "timeout_seconds": 180,
    "buffer_budget_bytes": 16777216,
    "use_write_session": false,
    "rigtorp_write_prefetch": false,
    "readerwriterqueue_copy_after_zero": true
  },
  "32 B / rigtorp": {
    "transport": "rigtorp",
    "mode": "throughput",
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
    "source_block_size": 65536,
    "source_pool_min": 128,
    "source_pool_max": 128,
    "source_queue_capacity": 512,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 65536,
    "output_frame_capacity": 1024,
    "output_pool_min": 128,
    "output_pool_max": 128,
    "output_queue_capacity": 512,
    "cancel_claim_mode": "tail",
    "output_mode": "spsc",
    "output_reader_mode": "wait_signal",
    "output_reader_count": 1,
    "output_retry_wait_us": 1000,
    "output_enable_stats": false,
    "is_scan_current": false,
    "reader_spin_count": 10000,
    "rescan_interval_ns": 5000000,
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
    "spsc_capacity": 262139,
    "retry_yield_every": 1024,
    "spsc_idle_spins": 0,
    "spsc_idle_sleep_ns": 1000,
    "timer": "auto",
    "sample_every": 32,
    "max_samples": 1000000,
    "latency_batch_size": 1000,
    "timer_calibration_samples": 16384,
    "call_batch_size": 256,
    "random_seed": 42,
    "timeout_seconds": 180,
    "buffer_budget_bytes": 16777216,
    "use_write_session": false,
    "rigtorp_write_prefetch": true,
    "readerwriterqueue_copy_after_zero": false
  },
  "32 B / atomic_queue": {
    "transport": "atomic_queue",
    "mode": "throughput",
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
    "source_block_size": 65536,
    "source_pool_min": 128,
    "source_pool_max": 128,
    "source_queue_capacity": 512,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 65536,
    "output_frame_capacity": 1024,
    "output_pool_min": 128,
    "output_pool_max": 128,
    "output_queue_capacity": 512,
    "cancel_claim_mode": "tail",
    "output_mode": "spsc",
    "output_reader_mode": "wait_signal",
    "output_reader_count": 1,
    "output_retry_wait_us": 1000,
    "output_enable_stats": false,
    "is_scan_current": false,
    "reader_spin_count": 10000,
    "rescan_interval_ns": 5000000,
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
    "spsc_capacity": 262144,
    "retry_yield_every": 1024,
    "spsc_idle_spins": 0,
    "spsc_idle_sleep_ns": 10000,
    "timer": "auto",
    "sample_every": 32,
    "max_samples": 1000000,
    "latency_batch_size": 1000,
    "timer_calibration_samples": 16384,
    "call_batch_size": 256,
    "random_seed": 42,
    "timeout_seconds": 180,
    "buffer_budget_bytes": 16777216,
    "use_write_session": false,
    "rigtorp_write_prefetch": false,
    "readerwriterqueue_copy_after_zero": false
  },
  "64 B / two_buf_queue": {
    "transport": "two_buf_queue",
    "mode": "throughput",
    "payload_bytes": 64,
    "messages": 10000000,
    "warmup_messages": 50000,
    "callback_iterations": 0,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 4194304,
    "source_pool_min": 2,
    "source_pool_max": 2,
    "source_queue_capacity": 8,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 4194304,
    "output_frame_capacity": 1024,
    "output_pool_min": 2,
    "output_pool_max": 2,
    "output_queue_capacity": 8,
    "cancel_claim_mode": "tail",
    "output_mode": "spsc",
    "output_reader_mode": "wait_signal",
    "output_reader_count": 1,
    "output_retry_wait_us": 1000,
    "output_enable_stats": false,
    "is_scan_current": true,
    "reader_spin_count": 10000,
    "rescan_interval_ns": 1000000,
    "rescan_max_interval_ns": 1000000,
    "consumer_cpu": -1,
    "producer_cpu": -1,
    "producer_qos": "user_interactive",
    "round_storage_bytes": 6710886,
    "spsc_buffer_bytes": 8388608,
    "prefault_producer_buffers": true,
    "finish_timeout_ms": 5000,
    "work_wait_min_ms": 1,
    "finish_retry_ms": 1,
    "publish_count": 10,
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
    "random_seed": 42,
    "timeout_seconds": 180,
    "buffer_budget_bytes": 16777216,
    "use_write_session": false,
    "rigtorp_write_prefetch": false,
    "readerwriterqueue_copy_after_zero": false
  },
  "64 B / readerwriterqueue": {
    "transport": "readerwriterqueue",
    "mode": "throughput",
    "payload_bytes": 64,
    "messages": 10000000,
    "warmup_messages": 50000,
    "callback_iterations": 0,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 65536,
    "source_pool_min": 128,
    "source_pool_max": 128,
    "source_queue_capacity": 512,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 65536,
    "output_frame_capacity": 1024,
    "output_pool_min": 128,
    "output_pool_max": 128,
    "output_queue_capacity": 512,
    "cancel_claim_mode": "tail",
    "output_mode": "spsc",
    "output_reader_mode": "wait_signal",
    "output_reader_count": 1,
    "output_retry_wait_us": 1000,
    "output_enable_stats": false,
    "is_scan_current": false,
    "reader_spin_count": 10000,
    "rescan_interval_ns": 5000000,
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
    "spsc_capacity": 130305,
    "retry_yield_every": 1024,
    "spsc_idle_spins": 0,
    "spsc_idle_sleep_ns": 10000,
    "timer": "auto",
    "sample_every": 32,
    "max_samples": 1000000,
    "latency_batch_size": 1000,
    "timer_calibration_samples": 16384,
    "call_batch_size": 256,
    "random_seed": 42,
    "timeout_seconds": 180,
    "buffer_budget_bytes": 16777216,
    "use_write_session": false,
    "rigtorp_write_prefetch": false,
    "readerwriterqueue_copy_after_zero": false
  },
  "64 B / rigtorp": {
    "transport": "rigtorp",
    "mode": "throughput",
    "payload_bytes": 64,
    "messages": 10000000,
    "warmup_messages": 50000,
    "callback_iterations": 0,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 65536,
    "source_pool_min": 128,
    "source_pool_max": 128,
    "source_queue_capacity": 512,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 65536,
    "output_frame_capacity": 1024,
    "output_pool_min": 128,
    "output_pool_max": 128,
    "output_queue_capacity": 512,
    "cancel_claim_mode": "tail",
    "output_mode": "spsc",
    "output_reader_mode": "wait_signal",
    "output_reader_count": 1,
    "output_retry_wait_us": 1000,
    "output_enable_stats": false,
    "is_scan_current": false,
    "reader_spin_count": 10000,
    "rescan_interval_ns": 5000000,
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
    "spsc_capacity": 131069,
    "retry_yield_every": 1024,
    "spsc_idle_spins": 0,
    "spsc_idle_sleep_ns": 1000,
    "timer": "auto",
    "sample_every": 32,
    "max_samples": 1000000,
    "latency_batch_size": 1000,
    "timer_calibration_samples": 16384,
    "call_batch_size": 256,
    "random_seed": 42,
    "timeout_seconds": 180,
    "buffer_budget_bytes": 16777216,
    "use_write_session": false,
    "rigtorp_write_prefetch": true,
    "readerwriterqueue_copy_after_zero": false
  },
  "64 B / atomic_queue": {
    "transport": "atomic_queue",
    "mode": "throughput",
    "payload_bytes": 64,
    "messages": 10000000,
    "warmup_messages": 50000,
    "callback_iterations": 0,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 65536,
    "source_pool_min": 128,
    "source_pool_max": 128,
    "source_queue_capacity": 512,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 65536,
    "output_frame_capacity": 1024,
    "output_pool_min": 128,
    "output_pool_max": 128,
    "output_queue_capacity": 512,
    "cancel_claim_mode": "tail",
    "output_mode": "spsc",
    "output_reader_mode": "wait_signal",
    "output_reader_count": 1,
    "output_retry_wait_us": 1000,
    "output_enable_stats": false,
    "is_scan_current": false,
    "reader_spin_count": 10000,
    "rescan_interval_ns": 5000000,
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
    "spsc_capacity": 131072,
    "retry_yield_every": 1024,
    "spsc_idle_spins": 0,
    "spsc_idle_sleep_ns": 10000,
    "timer": "auto",
    "sample_every": 32,
    "max_samples": 1000000,
    "latency_batch_size": 1000,
    "timer_calibration_samples": 16384,
    "call_batch_size": 256,
    "random_seed": 42,
    "timeout_seconds": 180,
    "buffer_budget_bytes": 16777216,
    "use_write_session": false,
    "rigtorp_write_prefetch": false,
    "readerwriterqueue_copy_after_zero": false
  }
}
```

生成于 2026-10-11T11:17:20.030789+08:00 · 共 160 次正式运行，16 次负载校验，0 次回调校准。

编译器：Clang 18.1.8

被测程序 SHA-256：d1194d84f55ad72ea9a0445bfba2d1bbedab8953ff494b928b23334856386e23

[reproduction.json](../../performance_data/normal_current_scan10_20261011/8m_normal/reproduction.json) · [results.json](../../performance_data/normal_current_scan10_20261011/8m_normal/results.json) · [raw.jsonl](../../performance_data/normal_current_scan10_20261011/8m_normal/raw.jsonl) · [profile.json](../../performance_data/normal_current_scan10_20261011/8m_normal/profile.json) · [environment.json](../../performance_data/normal_current_scan10_20261011/8m_normal/environment.json) · [audit.json](../../performance_data/normal_current_scan10_20261011/8m_normal/audit.json) · [dependencies.json](../../performance_data/normal_current_scan10_20261011/8m_normal/dependencies.json)

第三方原文件按 benchmarks/spsc/spsc_dependencies.json 校验，未修改队列实现；外部 Payload 的复制策略为 完整负载复制；rigtorp 可选目标写预取，ReaderWriterQueue 可选先清零再 memcpy，复制完整负载。逐次命令、配置、校验结果与内部计数保存在 raw.jsonl。

此 HTML 内嵌汇总数据，离线可查看、筛选与导出。原始日志链接需要同目录下的 performance_data 文件夹。
