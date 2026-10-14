TWOBUFQUEUE / PERFORMANCE NOTES

# 256 MiB 缓存 · 回调循环固定 · 普通 write

固定长度、不写长度前缀的 TwoBufQueue，与三种开源 SPSC 队列对比。TwoBufQueue 包含完整第二层复制与独立回调线程；第三方使用原生消费线程逐条回调。

10 百万条 / 次 5 次平均 / 指标 8 B / 16 B / 32 B / 64 B publish_count = 10 扫描当前块 · 不主动 flush 高 QoS · 未硬绑核

[总览](#按消息长度查看吞吐) · [吞吐与延时](#吞吐与写入耗时) · [时间与背压](#回调时间与背压) · [原始值](#逐次测量值) · [测试口径](#测试口径与复现信息)

其他报告：[256M Session](performance_256m_callback_session.md) · [8M 普通](performance_8m_zero.md) · [8M Session](performance_8m_zero_session.md)

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

TwoBufQueue 的第一层与第二层各 256 MiB，合计 512 MiB；三种开源 SPSC 请求 256 MiB。ReaderWriterQueue 会按内部块容量略微取整。

每种长度的回调业务循环次数固定，各实现共享；本轮未重新校准到 2～3 倍，表格保留实际时间比。

本次没有重新寻优，吞吐和各延时指标使用同一构造配置。普通 write 与 Session 的读端/发布配置和测量轮次可能不同，不能当作同参数对照。

只重新测量 normal 与三个开源对照；另一写模式的报告保持原样，没有把它的历史数据混入本轮主表。

每层数据池严格为 256 MiB；原生队列的保留槽与 rigtorp 首尾 padding 纳入这个 payload 存储预算，队列元数据和输入另计。

Session 正常遇到块满返回 insufficient_space，次数记入 session_block_exhaustions；这不是背压或丢消息。池/队列受压通过 Writer 内部压力统计报告。

rigtorp_write_prefetch 在完整参数中记录；开关在测试开始前选择模板，普通复制和带提示构造都只复制一次完整负载，不额外写零。atomic_queue 保持原有调用。

ReaderWriterQueue 16/32 B 使用 copy_after_zero：队列槽位默认清零后 memcpy 一次完整负载；8/64 B 保留成员复制。清零和复制均在写入计时内。各实现的调用端优化在行标签和完整参数中公开，不能把所有差异归因于队列算法。

256 MiB 回调循环按 8/16/32/64 B 固定为 1/1/2/4 次，本次未重新校准。用于控制读端/发布参数变化；实际时间比保留在表格中，不保证仍在 2～3 倍范围。

TwoBufQueue 8 B：is_scan_current=true, publish_count=10, 初始/最大扫描间隔=1000000/1000000 ns，最小等待=1 ms。

TwoBufQueue 16 B：is_scan_current=true, publish_count=10, 初始/最大扫描间隔=1000000/1000000 ns，最小等待=1 ms。

TwoBufQueue 32 B：is_scan_current=true, publish_count=10, 初始/最大扫描间隔=1000000/1000000 ns，最小等待=1 ms。

TwoBufQueue 64 B：is_scan_current=true, publish_count=10, 初始/最大扫描间隔=1000000/1000000 ns，最小等待=1 ms。

本轮只改变普通 write 的当前块扫描和发布参数；Session 的配置、报告和原始结果保持上一轮原样。三个开源对照本轮重新测量，未使用历史成绩替换。

256 MiB 的 1/1/2/4 次回调循环沿用 release_20261011_inline_fixed_110024；8 MiB 仍为 0。本次合计 320 次正式测量、32 次独立完整负载/FIFO 校验，没有重新校准或寻优。

测试框架增加 publish_count=10 的编译期 Writer 实例，并修正非 2 次幂发布次数的轮末自然发布对齐；未修改库 src/include 或第三方头文件。每轮少于 1000 条的尾部仍发送并记录，延时样本只统计完整的 1000 次写入组。

消息长度

8 B 吞吐 · M条/s

TwoBufQueue 1,195.91

ReaderWriterQueue 833.85

rigtorp/SPSCQueue 311.29

atomic_queue 184.54

16 B 吞吐 · M条/s

TwoBufQueue 741.87

ReaderWriterQueue 717.45

rigtorp/SPSCQueue 275.15

atomic_queue 127.85

32 B 吞吐 · M条/s

TwoBufQueue 135.68

ReaderWriterQueue 437.55

rigtorp/SPSCQueue 195.21

atomic_queue 76.91

64 B 吞吐 · M条/s

TwoBufQueue 93.64

ReaderWriterQueue 149.89

rigtorp/SPSCQueue 118.20

atomic_queue 71.73

## 吞吐与写入耗时

每个指标先按单次运行计算，再取 5 次算术平均。TwoBufQueue 整行加粗并加边框。

**8 B 回调循环 1 次 · 每次 1 轮 · 10,000,000 条**

| 实现 | 吞吐 · M条/s · 下方为范围 | 有效负载 · GB/s | p50 · ns / 次 | p99 · ns / 次 | p999 · ns / 次 | 回调区间 / 写入 · 墙钟时间比 | 回调循环 · 次 / 消息 | 分轮 · 轮 / 运行 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 普通 write · 固定长度写** | **1,195.91 · 1,132.83 – 1,243.06** | **9.567** | **0.808** | **1.425** | **3.033** | **3.38×** | **1** | **1** |
| ReaderWriterQueue · 成员初始化复制 | 833.85 · 746.29 – 876.01 | 6.671 | 1.250 | 2.217 | 4.575 | 3.36× | 1 | 1 |
| rigtorp/SPSCQueue · 成员复制＋目标写预取 | 311.29 · 296.52 – 333.86 | 2.490 | 3.100 | 5.467 | 9.967 | 1.00× | 1 | 1 |
| atomic_queue · 成员初始化复制 | 184.54 · 178.10 – 191.21 | 1.476 | 5.083 | 14.258 | 38.000 | 1.18× | 1 | 1 |

**16 B 回调循环 1 次 · 每次 1 轮 · 10,000,000 条**

| 实现 | 吞吐 · M条/s · 下方为范围 | 有效负载 · GB/s | p50 · ns / 次 | p99 · ns / 次 | p999 · ns / 次 | 回调区间 / 写入 · 墙钟时间比 | 回调循环 · 次 / 消息 | 分轮 · 轮 / 运行 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 普通 write · 固定长度写** | **741.87 · 514.21 – 1,025.22** | **11.870** | **0.917** | **5.675** | **28.383** | **2.66×** | **1** | **1** |
| ReaderWriterQueue · 先清零，再 memcpy | 717.45 · 609.02 – 768.41 | 11.479 | 1.308 | 3.125 | 6.383 | 3.24× | 1 | 1 |
| rigtorp/SPSCQueue · 成员复制＋目标写预取 | 275.15 · 231.80 – 312.00 | 4.402 | 3.450 | 7.600 | 14.242 | 1.00× | 1 | 1 |
| atomic_queue · 成员初始化复制 | 127.85 · 123.88 – 131.99 | 2.046 | 6.008 | 21.967 | 31.658 | 1.01× | 1 | 1 |

**32 B 回调循环 2 次 · 每次 2 轮 · 10,000,000 条**

| 实现 | 吞吐 · M条/s · 下方为范围 | 有效负载 · GB/s | p50 · ns / 次 | p99 · ns / 次 | p999 · ns / 次 | 回调区间 / 写入 · 墙钟时间比 | 回调循环 · 次 / 消息 | 分轮 · 轮 / 运行 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 普通 write · 固定长度写** | **135.68 · 120.01 – 153.22** | **4.342** | **5.208** | **12.750** | **26.758** | **1.02×** | **2** | **2** |
| ReaderWriterQueue · 先清零，再 memcpy | 437.55 · 302.80 – 506.68 | 14.002 | 2.417 | 19.667 | 120.708 | 2.30× | 2 | 2 |
| rigtorp/SPSCQueue · 成员复制＋目标写预取 | 195.21 · 117.60 – 232.95 | 6.247 | 4.750 | 9.733 | 15.442 | 1.00× | 2 | 2 |
| atomic_queue · 成员初始化复制 | 76.91 · 70.95 – 82.79 | 2.461 | 9.600 | 27.833 | 42.033 | 1.00× | 2 | 2 |

**64 B 回调循环 4 次 · 每次 3 轮 · 10,000,000 条**

| 实现 | 吞吐 · M条/s · 下方为范围 | 有效负载 · GB/s | p50 · ns / 次 | p99 · ns / 次 | p999 · ns / 次 | 回调区间 / 写入 · 墙钟时间比 | 回调循环 · 次 / 消息 | 分轮 · 轮 / 运行 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 普通 write · 固定长度写** | **93.64 · 93.10 – 94.03** | **5.993** | **9.933** | **20.683** | **28.433** | **1.02×** | **4** | **3** |
| ReaderWriterQueue · 成员初始化复制 | 149.89 · 71.92 – 220.25 | 9.593 | 4.833 | 44.575 | 182.250 | 1.48× | 4 | 3 |
| rigtorp/SPSCQueue · 成员复制＋目标写预取 | 118.20 · 107.18 – 124.10 | 7.565 | 8.225 | 19.758 | 144.417 | 1.00× | 4 | 3 |
| atomic_queue · 成员初始化复制 | 71.73 · 68.61 – 74.29 | 4.591 | 10.858 | 25.917 | 48.467 | 1.00× | 4 | 3 |

p50 / p99 / p999 是“每组 1000 次成功写入的平均耗时”的分位数，不是单条消息端到端延时，也不是单次 write 的分位数。吞吐使用独立的未逐组计时运行。GB/s 按业务净负载计算。时间比 = 各轮首条回调开始至末条回调结束的耗时之和 ÷ 各轮写线程发送耗时之和，再对运行取平均；它包含分发、等待数据和调度，不是每次回调函数 CPU 耗时之和。

## 回调时间与背压

与吞吐表使用同一批正式运行。时间、次数按运行取平均；峰值 RSS 为这组运行的最大值。

**8 B 正式吞吐运行的逐次平均；RSS 为本组最大值**

| 实现 | 写线程 · ms | 回调区间 · ms | 受压消息 · 条 / 次运行 | 失败尝试 · 次 / 运行 | 重试时间 · ms | Writer 压力 · 内部事件 | 复制层拒绝 · 内部事件 | 峰值 RSS · MiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue** | **8.37** | **28.27** | **0.0** | **0.0** | **0.000** | **0** | **0** | **394.9** |
| ReaderWriterQueue | 12.04 | 40.33 | 0.0 | 0.0 | 0.000 | — | — | 367.3 |
| rigtorp/SPSCQueue | 32.18 | 32.18 | 0.0 | 0.0 | 0.000 | — | — | 334.0 |
| atomic_queue | 54.22 | 63.91 | 0.0 | 0.0 | 0.000 | — | — | 366.0 |

**16 B 正式吞吐运行的逐次平均；RSS 为本组最大值**

| 实现 | 写线程 · ms | 回调区间 · ms | 受压消息 · 条 / 次运行 | 失败尝试 · 次 / 运行 | 重试时间 · ms | Writer 压力 · 内部事件 | 复制层拒绝 · 内部事件 | 峰值 RSS · MiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue** | **14.69** | **36.46** | **0.0** | **0.0** | **0.000** | **0** | **0** | **544.9** |
| ReaderWriterQueue | 14.03 | 45.09 | 0.0 | 0.0 | 0.000 | — | — | 427.7 |
| rigtorp/SPSCQueue | 36.70 | 36.70 | 0.0 | 0.0 | 0.000 | — | — | 410.3 |
| atomic_queue | 78.25 | 79.11 | 0.0 | 0.0 | 0.000 | — | — | 426.3 |

**32 B 正式吞吐运行的逐次平均；RSS 为本组最大值**

| 实现 | 写线程 · ms | 回调区间 · ms | 受压消息 · 条 / 次运行 | 失败尝试 · 次 / 运行 | 重试时间 · ms | Writer 压力 · 内部事件 | 复制层拒绝 · 内部事件 | 峰值 RSS · MiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue** | **74.15** | **75.26** | **0.0** | **0.0** | **0.000** | **0** | **0** | **480.4** |
| ReaderWriterQueue | 23.74 | 52.79 | 0.0 | 0.0 | 0.000 | — | — | 471.9 |
| rigtorp/SPSCQueue | 54.41 | 54.41 | 0.0 | 0.0 | 0.000 | — | — | 462.5 |
| atomic_queue | 130.51 | 130.56 | 0.0 | 0.0 | 0.000 | — | — | 470.5 |

**64 B 正式吞吐运行的逐次平均；RSS 为本组最大值**

| 实现 | 写线程 · ms | 回调区间 · ms | 受压消息 · 条 / 次运行 | 失败尝试 · 次 / 运行 | 重试时间 · ms | Writer 压力 · 内部事件 | 复制层拒绝 · 内部事件 | 峰值 RSS · MiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue** | **106.80** | **108.82** | **0.0** | **0.0** | **0.000** | **0** | **0** | **680.9** |
| ReaderWriterQueue | 81.17 | 108.41 | 0.0 | 0.0 | 0.000 | — | — | 590.7 |
| rigtorp/SPSCQueue | 84.83 | 84.83 | 0.0 | 0.0 | 0.000 | — | — | 462.6 |
| atomic_queue | 139.52 | 139.70 | 0.0 | 0.0 | 0.000 | — | — | 466.6 |

受压消息至少经历一次 try 写入失败；失败尝试含重复重试，最终仍全部写入。Writer 与复制层内部事件仅 TwoBufQueue 提供，其他实现显示“—”，不能把“—”解释为 0。回调区间含分发、轮内等待及调度，时间比大于 1 本身不等于写端背压。RSS 包含进程开销，不等于缓存容量。

## 逐次测量值

展开查看全部 5 次吞吐及分位数。没有剔除异常值，也没有将多个运行的样本合并计算分位数。

### 8 B 5 次原始值 · 回调循环 1 次



| 实现 / 指标 | 第 1 次 | 第 2 次 | 第 3 次 | 第 4 次 | 第 5 次 | 平均 |
| --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 吞吐 M条/s** | **1,212.709** | **1,243.060** | **1,132.834** | **1,235.859** | **1,155.101** | **1,195.913** |
| **TwoBufQueue · p50 ns** | **0.792** | **0.875** | **0.875** | **0.750** | **0.750** | **0.808** |
| **TwoBufQueue · p99 ns** | **1.458** | **1.500** | **1.625** | **1.542** | **1.000** | **1.425** |
| **TwoBufQueue · p999 ns** | **2.125** | **3.042** | **3.542** | **4.667** | **1.792** | **3.033** |
| ReaderWriterQueue · 吞吐 M条/s | 872.908 | 857.918 | 876.008 | 746.287 | 816.129 | 833.850 |
| ReaderWriterQueue · p50 ns | 1.250 | 1.250 | 1.250 | 1.250 | 1.250 | 1.250 |
| ReaderWriterQueue · p99 ns | 1.958 | 1.917 | 2.333 | 2.625 | 2.250 | 2.217 |
| ReaderWriterQueue · p999 ns | 4.042 | 4.792 | 3.167 | 7.000 | 3.875 | 4.575 |
| rigtorp/SPSCQueue · 吞吐 M条/s | 333.856 | 296.523 | 304.406 | 312.545 | 309.096 | 311.285 |
| rigtorp/SPSCQueue · p50 ns | 3.000 | 2.958 | 3.375 | 3.042 | 3.125 | 3.100 |
| rigtorp/SPSCQueue · p99 ns | 4.667 | 4.250 | 6.667 | 5.542 | 6.208 | 5.467 |
| rigtorp/SPSCQueue · p999 ns | 9.292 | 7.625 | 12.958 | 8.792 | 11.167 | 9.967 |
| atomic_queue · 吞吐 M条/s | 184.711 | 187.305 | 191.214 | 178.100 | 181.375 | 184.541 |
| atomic_queue · p50 ns | 4.875 | 5.333 | 5.000 | 5.167 | 5.042 | 5.083 |
| atomic_queue · p99 ns | 7.208 | 7.625 | 39.708 | 8.583 | 8.167 | 14.258 |
| atomic_queue · p999 ns | 36.583 | 36.583 | 43.958 | 37.625 | 35.250 | 38.000 |

### 16 B 5 次原始值 · 回调循环 1 次



| 实现 / 指标 | 第 1 次 | 第 2 次 | 第 3 次 | 第 4 次 | 第 5 次 | 平均 |
| --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 吞吐 M条/s** | **995.780** | **514.208** | **1,025.220** | **624.096** | **550.036** | **741.868** |
| **TwoBufQueue · p50 ns** | **0.958** | **0.917** | **0.958** | **0.875** | **0.875** | **0.917** |
| **TwoBufQueue · p99 ns** | **3.875** | **14.167** | **3.583** | **2.458** | **4.292** | **5.675** |
| **TwoBufQueue · p999 ns** | **12.125** | **105.250** | **9.708** | **6.083** | **8.750** | **28.383** |
| ReaderWriterQueue · 吞吐 M条/s | 725.422 | 731.814 | 609.023 | 752.596 | 768.408 | 717.453 |
| ReaderWriterQueue · p50 ns | 1.250 | 1.292 | 1.250 | 1.292 | 1.458 | 1.308 |
| ReaderWriterQueue · p99 ns | 2.333 | 5.125 | 2.042 | 2.708 | 3.417 | 3.125 |
| ReaderWriterQueue · p999 ns | 3.208 | 10.167 | 2.792 | 8.500 | 7.250 | 6.383 |
| rigtorp/SPSCQueue · 吞吐 M条/s | 273.566 | 290.803 | 231.799 | 312.002 | 267.590 | 275.152 |
| rigtorp/SPSCQueue · p50 ns | 3.208 | 3.208 | 3.167 | 3.250 | 4.417 | 3.450 |
| rigtorp/SPSCQueue · p99 ns | 6.292 | 6.750 | 5.375 | 5.667 | 13.917 | 7.600 |
| rigtorp/SPSCQueue · p999 ns | 9.417 | 12.750 | 8.125 | 9.333 | 31.583 | 14.242 |
| atomic_queue · 吞吐 M条/s | 131.995 | 123.879 | 129.033 | 127.571 | 126.793 | 127.854 |
| atomic_queue · p50 ns | 6.000 | 6.000 | 6.292 | 5.875 | 5.875 | 6.008 |
| atomic_queue · p99 ns | 23.500 | 24.375 | 22.333 | 16.292 | 23.333 | 21.967 |
| atomic_queue · p999 ns | 33.375 | 43.417 | 29.000 | 20.792 | 31.708 | 31.658 |

### 32 B 5 次原始值 · 回调循环 2 次



| 实现 / 指标 | 第 1 次 | 第 2 次 | 第 3 次 | 第 4 次 | 第 5 次 | 平均 |
| --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 吞吐 M条/s** | **137.007** | **120.007** | **133.870** | **134.292** | **153.221** | **135.680** |
| **TwoBufQueue · p50 ns** | **6.417** | **4.458** | **5.625** | **4.500** | **5.042** | **5.208** |
| **TwoBufQueue · p99 ns** | **17.542** | **11.250** | **11.917** | **11.208** | **11.833** | **12.750** |
| **TwoBufQueue · p999 ns** | **56.292** | **15.667** | **28.250** | **13.667** | **19.917** | **26.758** |
| ReaderWriterQueue · 吞吐 M条/s | 474.117 | 302.797 | 399.291 | 504.881 | 506.680 | 437.553 |
| ReaderWriterQueue · p50 ns | 3.167 | 3.208 | 1.958 | 1.875 | 1.875 | 2.417 |
| ReaderWriterQueue · p99 ns | 80.042 | 6.000 | 3.958 | 4.250 | 4.083 | 19.667 |
| ReaderWriterQueue · p999 ns | 559.583 | 12.917 | 8.167 | 11.417 | 11.458 | 120.708 |
| rigtorp/SPSCQueue · 吞吐 M条/s | 206.909 | 218.431 | 117.596 | 232.950 | 200.141 | 195.205 |
| rigtorp/SPSCQueue · p50 ns | 4.167 | 4.167 | 5.542 | 4.250 | 5.625 | 4.750 |
| rigtorp/SPSCQueue · p99 ns | 8.083 | 12.917 | 9.333 | 8.083 | 10.250 | 9.733 |
| rigtorp/SPSCQueue · p999 ns | 12.625 | 20.375 | 14.833 | 12.000 | 17.375 | 15.442 |
| atomic_queue · 吞吐 M条/s | 72.420 | 77.311 | 82.787 | 81.060 | 70.946 | 76.905 |
| atomic_queue · p50 ns | 9.167 | 10.042 | 10.042 | 8.667 | 10.083 | 9.600 |
| atomic_queue · p99 ns | 26.958 | 30.000 | 27.583 | 26.542 | 28.083 | 27.833 |
| atomic_queue · p999 ns | 33.500 | 47.292 | 55.708 | 29.833 | 43.833 | 42.033 |

### 64 B 5 次原始值 · 回调循环 4 次



| 实现 / 指标 | 第 1 次 | 第 2 次 | 第 3 次 | 第 4 次 | 第 5 次 | 平均 |
| --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 吞吐 M条/s** | **93.488** | **93.988** | **93.569** | **94.032** | **93.103** | **93.636** |
| **TwoBufQueue · p50 ns** | **9.917** | **10.000** | **9.917** | **9.875** | **9.958** | **9.933** |
| **TwoBufQueue · p99 ns** | **21.583** | **19.167** | **20.208** | **20.917** | **21.542** | **20.683** |
| **TwoBufQueue · p999 ns** | **27.125** | **27.958** | **24.500** | **36.750** | **25.833** | **28.433** |
| ReaderWriterQueue · 吞吐 M条/s | 88.690 | 71.919 | 177.922 | 190.661 | 220.251 | 149.889 |
| ReaderWriterQueue · p50 ns | 4.292 | 4.375 | 6.208 | 4.667 | 4.625 | 4.833 |
| ReaderWriterQueue · p99 ns | 9.625 | 12.667 | 24.292 | 163.292 | 13.000 | 44.575 |
| ReaderWriterQueue · p999 ns | 15.333 | 19.875 | 220.333 | 630.875 | 24.833 | 182.250 |
| rigtorp/SPSCQueue · 吞吐 M条/s | 124.102 | 122.488 | 107.183 | 117.795 | 119.436 | 118.201 |
| rigtorp/SPSCQueue · p50 ns | 7.917 | 8.583 | 8.000 | 7.875 | 8.750 | 8.225 |
| rigtorp/SPSCQueue · p99 ns | 12.750 | 45.042 | 14.208 | 12.500 | 14.292 | 19.758 |
| rigtorp/SPSCQueue · p999 ns | 16.792 | 633.250 | 25.708 | 19.542 | 26.792 | 144.417 |
| atomic_queue · 吞吐 M条/s | 70.500 | 72.363 | 72.882 | 68.614 | 74.293 | 71.731 |
| atomic_queue · p50 ns | 10.750 | 10.792 | 10.625 | 11.333 | 10.792 | 10.858 |
| atomic_queue · p99 ns | 25.833 | 25.125 | 25.792 | 28.167 | 24.667 | 25.917 |
| atomic_queue · p999 ns | 50.417 | 41.000 | 41.875 | 70.125 | 38.917 | 48.467 |

波动较大的吞吐组：16 B · TwoBufQueue：514.21–1,025.22 M条/s；16 B · ReaderWriterQueue：609.02–768.41 M条/s；16 B · rigtorp/SPSCQueue：231.80–312.00 M条/s；32 B · TwoBufQueue：120.01–153.22 M条/s；32 B · ReaderWriterQueue：302.80–506.68 M条/s；32 B · rigtorp/SPSCQueue：117.60–232.95 M条/s；64 B · ReaderWriterQueue：71.92–220.25 M条/s。所有测量均保留，未删除低值。

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

**p999 仅供观察：** 每次延时测量约有 9,998–10,000 个组均值样本，少于框架建议的 100,000 个。p999 的尾部覆盖有限，不能据此做稳定的尾延时承诺。p999 表示 99.9% 分位数；平均值不代表置信区间。

### 完整构造参数 · 每种长度 / 实现 / 优化指标



```
{
  "8 B / two_buf_queue": {
    "transport": "two_buf_queue",
    "mode": "throughput",
    "payload_bytes": 8,
    "messages": 10000000,
    "warmup_messages": 50000,
    "callback_iterations": 1,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 1048576,
    "source_pool_min": 256,
    "source_pool_max": 256,
    "source_queue_capacity": 1024,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 1048576,
    "output_frame_capacity": 1,
    "output_pool_min": 256,
    "output_pool_max": 256,
    "output_queue_capacity": 1024,
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
    "round_storage_bytes": 214748364,
    "spsc_buffer_bytes": 268435456,
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
    "buffer_budget_bytes": 536870912,
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
    "callback_iterations": 1,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 1048576,
    "source_pool_min": 256,
    "source_pool_max": 256,
    "source_queue_capacity": 1024,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 1048576,
    "output_frame_capacity": 1024,
    "output_pool_min": 256,
    "output_pool_max": 256,
    "output_queue_capacity": 1024,
    "cancel_claim_mode": "tail",
    "output_mode": "spsc",
    "output_reader_mode": "wait_signal",
    "output_reader_count": 1,
    "output_retry_wait_us": 1000,
    "output_enable_stats": false,
    "is_scan_current": false,
    "reader_spin_count": 1000,
    "rescan_interval_ns": 1000000,
    "rescan_max_interval_ns": 30000000,
    "consumer_cpu": -1,
    "producer_cpu": -1,
    "producer_qos": "user_interactive",
    "round_storage_bytes": 214748364,
    "spsc_buffer_bytes": 268435456,
    "prefault_producer_buffers": true,
    "finish_timeout_ms": 5000,
    "work_wait_min_ms": 1,
    "finish_retry_ms": 1,
    "publish_count": 4294967295,
    "spsc_capacity": 33488385,
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
    "buffer_budget_bytes": 536870912,
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
    "callback_iterations": 1,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 1048576,
    "source_pool_min": 256,
    "source_pool_max": 256,
    "source_queue_capacity": 1024,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 1048576,
    "output_frame_capacity": 1024,
    "output_pool_min": 256,
    "output_pool_max": 256,
    "output_queue_capacity": 1024,
    "cancel_claim_mode": "tail",
    "output_mode": "spsc",
    "output_reader_mode": "wait_signal",
    "output_reader_count": 1,
    "output_retry_wait_us": 1000,
    "output_enable_stats": false,
    "is_scan_current": false,
    "reader_spin_count": 1000,
    "rescan_interval_ns": 1000000,
    "rescan_max_interval_ns": 30000000,
    "consumer_cpu": -1,
    "producer_cpu": -1,
    "producer_qos": "user_interactive",
    "round_storage_bytes": 214748364,
    "spsc_buffer_bytes": 268435456,
    "prefault_producer_buffers": true,
    "finish_timeout_ms": 5000,
    "work_wait_min_ms": 1,
    "finish_retry_ms": 1,
    "publish_count": 4294967295,
    "spsc_capacity": 33554415,
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
    "buffer_budget_bytes": 536870912,
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
    "callback_iterations": 1,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 1048576,
    "source_pool_min": 256,
    "source_pool_max": 256,
    "source_queue_capacity": 1024,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 1048576,
    "output_frame_capacity": 1024,
    "output_pool_min": 256,
    "output_pool_max": 256,
    "output_queue_capacity": 1024,
    "cancel_claim_mode": "tail",
    "output_mode": "spsc",
    "output_reader_mode": "wait_signal",
    "output_reader_count": 1,
    "output_retry_wait_us": 1000,
    "output_enable_stats": false,
    "is_scan_current": false,
    "reader_spin_count": 1000,
    "rescan_interval_ns": 1000000,
    "rescan_max_interval_ns": 30000000,
    "consumer_cpu": -1,
    "producer_cpu": -1,
    "producer_qos": "user_interactive",
    "round_storage_bytes": 214748364,
    "spsc_buffer_bytes": 268435456,
    "prefault_producer_buffers": true,
    "finish_timeout_ms": 5000,
    "work_wait_min_ms": 1,
    "finish_retry_ms": 1,
    "publish_count": 4294967295,
    "spsc_capacity": 33554432,
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
    "buffer_budget_bytes": 536870912,
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
    "callback_iterations": 1,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 262144,
    "source_pool_min": 1024,
    "source_pool_max": 1024,
    "source_queue_capacity": 4096,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 262144,
    "output_frame_capacity": 1024,
    "output_pool_min": 1024,
    "output_pool_max": 1024,
    "output_queue_capacity": 4096,
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
    "round_storage_bytes": 214748364,
    "spsc_buffer_bytes": 268435456,
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
    "buffer_budget_bytes": 536870912,
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
    "callback_iterations": 1,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 1048576,
    "source_pool_min": 256,
    "source_pool_max": 256,
    "source_queue_capacity": 1024,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 1048576,
    "output_frame_capacity": 1024,
    "output_pool_min": 256,
    "output_pool_max": 256,
    "output_queue_capacity": 1024,
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
    "round_storage_bytes": 214748364,
    "spsc_buffer_bytes": 268435456,
    "prefault_producer_buffers": true,
    "finish_timeout_ms": 5000,
    "work_wait_min_ms": 1,
    "finish_retry_ms": 1,
    "publish_count": 4294967295,
    "spsc_capacity": 16743937,
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
    "buffer_budget_bytes": 536870912,
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
    "callback_iterations": 1,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 1048576,
    "source_pool_min": 256,
    "source_pool_max": 256,
    "source_queue_capacity": 1024,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 1048576,
    "output_frame_capacity": 1024,
    "output_pool_min": 256,
    "output_pool_max": 256,
    "output_queue_capacity": 1024,
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
    "round_storage_bytes": 214748364,
    "spsc_buffer_bytes": 268435456,
    "prefault_producer_buffers": true,
    "finish_timeout_ms": 5000,
    "work_wait_min_ms": 1,
    "finish_retry_ms": 1,
    "publish_count": 4294967295,
    "spsc_capacity": 16777207,
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
    "buffer_budget_bytes": 536870912,
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
    "callback_iterations": 1,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 1048576,
    "source_pool_min": 256,
    "source_pool_max": 256,
    "source_queue_capacity": 1024,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 1048576,
    "output_frame_capacity": 1024,
    "output_pool_min": 256,
    "output_pool_max": 256,
    "output_queue_capacity": 1024,
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
    "round_storage_bytes": 214748364,
    "spsc_buffer_bytes": 268435456,
    "prefault_producer_buffers": true,
    "finish_timeout_ms": 5000,
    "work_wait_min_ms": 1,
    "finish_retry_ms": 1,
    "publish_count": 4294967295,
    "spsc_capacity": 16777216,
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
    "buffer_budget_bytes": 536870912,
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
    "callback_iterations": 2,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 65536,
    "source_pool_min": 4096,
    "source_pool_max": 4096,
    "source_queue_capacity": 16384,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 65536,
    "output_frame_capacity": 1,
    "output_pool_min": 4096,
    "output_pool_max": 4096,
    "output_queue_capacity": 16384,
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
    "round_storage_bytes": 214748364,
    "spsc_buffer_bytes": 268435456,
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
    "buffer_budget_bytes": 536870912,
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
    "callback_iterations": 2,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 65536,
    "source_pool_min": 4096,
    "source_pool_max": 4096,
    "source_queue_capacity": 16384,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 65536,
    "output_frame_capacity": 64,
    "output_pool_min": 4096,
    "output_pool_max": 4096,
    "output_queue_capacity": 16384,
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
    "round_storage_bytes": 214748364,
    "spsc_buffer_bytes": 268435456,
    "prefault_producer_buffers": true,
    "finish_timeout_ms": 5000,
    "work_wait_min_ms": 1,
    "finish_retry_ms": 1,
    "publish_count": 4294967295,
    "spsc_capacity": 8371713,
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
    "buffer_budget_bytes": 536870912,
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
    "callback_iterations": 2,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 65536,
    "source_pool_min": 4096,
    "source_pool_max": 4096,
    "source_queue_capacity": 16384,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 65536,
    "output_frame_capacity": 64,
    "output_pool_min": 4096,
    "output_pool_max": 4096,
    "output_queue_capacity": 16384,
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
    "round_storage_bytes": 214748364,
    "spsc_buffer_bytes": 268435456,
    "prefault_producer_buffers": true,
    "finish_timeout_ms": 5000,
    "work_wait_min_ms": 1,
    "finish_retry_ms": 1,
    "publish_count": 4294967295,
    "spsc_capacity": 8388603,
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
    "buffer_budget_bytes": 536870912,
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
    "callback_iterations": 2,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 65536,
    "source_pool_min": 4096,
    "source_pool_max": 4096,
    "source_queue_capacity": 16384,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 65536,
    "output_frame_capacity": 64,
    "output_pool_min": 4096,
    "output_pool_max": 4096,
    "output_queue_capacity": 16384,
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
    "round_storage_bytes": 214748364,
    "spsc_buffer_bytes": 268435456,
    "prefault_producer_buffers": true,
    "finish_timeout_ms": 5000,
    "work_wait_min_ms": 1,
    "finish_retry_ms": 1,
    "publish_count": 4294967295,
    "spsc_capacity": 8388608,
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
    "buffer_budget_bytes": 536870912,
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
    "callback_iterations": 4,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 1048576,
    "source_pool_min": 256,
    "source_pool_max": 256,
    "source_queue_capacity": 1024,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 1048576,
    "output_frame_capacity": 1,
    "output_pool_min": 256,
    "output_pool_max": 256,
    "output_queue_capacity": 1024,
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
    "round_storage_bytes": 214748364,
    "spsc_buffer_bytes": 268435456,
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
    "buffer_budget_bytes": 536870912,
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
    "callback_iterations": 4,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 262144,
    "source_pool_min": 1024,
    "source_pool_max": 1024,
    "source_queue_capacity": 4096,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 262144,
    "output_frame_capacity": 64,
    "output_pool_min": 1024,
    "output_pool_max": 1024,
    "output_queue_capacity": 4096,
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
    "round_storage_bytes": 214748364,
    "spsc_buffer_bytes": 268435456,
    "prefault_producer_buffers": true,
    "finish_timeout_ms": 5000,
    "work_wait_min_ms": 1,
    "finish_retry_ms": 1,
    "publish_count": 4294967295,
    "spsc_capacity": 4185601,
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
    "buffer_budget_bytes": 536870912,
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
    "callback_iterations": 4,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 262144,
    "source_pool_min": 1024,
    "source_pool_max": 1024,
    "source_queue_capacity": 4096,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 262144,
    "output_frame_capacity": 64,
    "output_pool_min": 1024,
    "output_pool_max": 1024,
    "output_queue_capacity": 4096,
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
    "round_storage_bytes": 214748364,
    "spsc_buffer_bytes": 268435456,
    "prefault_producer_buffers": true,
    "finish_timeout_ms": 5000,
    "work_wait_min_ms": 1,
    "finish_retry_ms": 1,
    "publish_count": 4294967295,
    "spsc_capacity": 4194301,
    "retry_yield_every": 1024,
    "spsc_idle_spins": 0,
    "spsc_idle_sleep_ns": 0,
    "timer": "auto",
    "sample_every": 32,
    "max_samples": 1000000,
    "latency_batch_size": 1000,
    "timer_calibration_samples": 16384,
    "call_batch_size": 256,
    "random_seed": 42,
    "timeout_seconds": 180,
    "buffer_budget_bytes": 536870912,
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
    "callback_iterations": 4,
    "verify_payload": false,
    "prepared_input": true,
    "variable_payload": true,
    "prepared_input_limit_bytes": 268435456,
    "measure_callback_time": true,
    "producer_interval_ns": 0,
    "source_block_size": 262144,
    "source_pool_min": 1024,
    "source_pool_max": 1024,
    "source_queue_capacity": 4096,
    "writer_backpressure": "drop",
    "writer_spin_count": 1000,
    "writer_wait_timeout_ms": 5000,
    "output_block_size": 262144,
    "output_frame_capacity": 64,
    "output_pool_min": 1024,
    "output_pool_max": 1024,
    "output_queue_capacity": 4096,
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
    "round_storage_bytes": 214748364,
    "spsc_buffer_bytes": 268435456,
    "prefault_producer_buffers": true,
    "finish_timeout_ms": 5000,
    "work_wait_min_ms": 1,
    "finish_retry_ms": 1,
    "publish_count": 4294967295,
    "spsc_capacity": 4194304,
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
    "buffer_budget_bytes": 536870912,
    "use_write_session": false,
    "rigtorp_write_prefetch": false,
    "readerwriterqueue_copy_after_zero": false
  }
}
```

生成于 2026-10-11T11:17:20.030789+08:00 · 共 160 次正式运行，16 次负载校验，0 次回调校准。

编译器：Clang 18.1.8

被测程序 SHA-256：d1194d84f55ad72ea9a0445bfba2d1bbedab8953ff494b928b23334856386e23

[reproduction.json](../../performance_data/normal_current_scan10_20261011/256m_normal/reproduction.json) · [results.json](../../performance_data/normal_current_scan10_20261011/256m_normal/results.json) · [raw.jsonl](../../performance_data/normal_current_scan10_20261011/256m_normal/raw.jsonl) · [profile.json](../../performance_data/normal_current_scan10_20261011/256m_normal/profile.json) · [environment.json](../../performance_data/normal_current_scan10_20261011/256m_normal/environment.json) · [audit.json](../../performance_data/normal_current_scan10_20261011/256m_normal/audit.json) · [dependencies.json](../../performance_data/normal_current_scan10_20261011/256m_normal/dependencies.json)

第三方原文件按 benchmarks/spsc/spsc_dependencies.json 校验，未修改队列实现；外部 Payload 的复制策略为 完整负载复制；rigtorp 可选目标写预取，ReaderWriterQueue 可选先清零再 memcpy，复制完整负载。逐次命令、配置、校验结果与内部计数保存在 raw.jsonl。

此 HTML 内嵌汇总数据，离线可查看、筛选与导出。原始日志链接需要同目录下的 performance_data 文件夹。
