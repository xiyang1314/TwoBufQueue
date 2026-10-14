TWOBUFQUEUE / PERFORMANCE NOTES

# 8 MiB 缓存 · 回调循环为 0 · 单块 Session 双重循环

固定长度、不写长度前缀的 TwoBufQueue，与三种开源 SPSC 队列对比。TwoBufQueue 包含完整第二层复制与独立回调线程；第三方使用原生消费线程逐条回调。

10 百万条 / 次 5 次平均 / 指标 8 B / 16 B / 32 B / 64 B publish_count = 4294967295 不逐条 flush · 轮末计时外交接 高 QoS · 未硬绑核

[总览](#按消息长度查看吞吐) · [吞吐与延时](#吞吐与写入耗时) · [时间与背压](#回调时间与背压) · [原始值](#逐次测量值) · [测试口径](#测试口径与复现信息)

其他报告：[256M 普通](performance_256m_callback.md) · [256M Session](performance_256m_callback_session.md) · [8M 普通](performance_8m_zero.md)

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

本次是普通 write 与单块 Session 的同参数对照，没有重新寻优。吞吐和各延时指标使用同一构造配置。

同缓存的两份报告共享本次实测的开源对照数据；没有使用历史成绩。

每层数据池严格为 8 MiB；原生队列的保留槽与 rigtorp 首尾 padding 纳入这个 payload 存储预算，队列元数据和输入另计。

Session 正常遇到块满返回 insufficient_space，次数记入 session_block_exhaustions；这不是背压或丢消息。池/队列受压通过 Writer 内部压力统计报告。

rigtorp_write_prefetch 在完整参数中记录；开关在测试开始前选择模板，普通复制和带提示构造都只复制一次完整负载，不额外写零。atomic_queue 保持原有调用。

ReaderWriterQueue 16/32 B 使用 copy_after_zero：队列槽位默认清零后 memcpy 一次完整负载；8/64 B 保留成员复制。清零和复制均在写入计时内。各实现的调用端优化在行标签和完整参数中公开，不能把所有差异归因于队列算法。

get_current_session → 内层逐条写 → 空间不足退出 → 外层 get_next_session。获取、等待、换块与 Session 状态归还包含在写入计时中。分组延时跨块继续计时，没有在块边界提前结束计时组。

不主动 flush；flush 只发布位置，不切换 block。每轮尾部在计时外 finish_block，最后一轮 finish，关闭 current 扫描也能收齐尾部。

本轮全部重新测量于提交 f698f25：只取消测试框架六个 lambda 的强制内联，Writer::write 的内联和库代码保持原样。8 B 回退的对照、历史源码与结论边界见 docs/performance_data/twobufqueue_8b_regression_20261011；本轮没有使用历史成绩替换。

本轮合计 400 次独立正式测量、40 次完整负载/FIFO 校验、45 次回调校准；同缓存的普通 write 与 Session 共享本轮开源数据，因此四份报告显示的运行次数不可直接相加。

消息长度

8 B 吞吐 · M条/s

TwoBufQueue 2,383.04

ReaderWriterQueue 813.31

rigtorp/SPSCQueue 323.04

atomic_queue 189.29

16 B 吞吐 · M条/s

TwoBufQueue 1,927.70

ReaderWriterQueue 681.12

rigtorp/SPSCQueue 317.50

atomic_queue 126.00

32 B 吞吐 · M条/s

TwoBufQueue 1,165.02

ReaderWriterQueue 498.11

rigtorp/SPSCQueue 258.44

atomic_queue 86.24

64 B 吞吐 · M条/s

TwoBufQueue 590.62

ReaderWriterQueue 242.06

rigtorp/SPSCQueue 161.12

atomic_queue 69.86

## 吞吐与写入耗时

每个指标先按单次运行计算，再取 5 次算术平均。TwoBufQueue 整行加粗并加边框。

**8 B 回调循环 0 次 · 每次 12 轮 · 10,000,000 条**

| 实现 | 吞吐 · M条/s · 下方为范围 | 有效负载 · GB/s | p50 · ns / 次 | p99 · ns / 次 | p999 · ns / 次 | 回调区间 / 写入 · 墙钟时间比 | 回调循环 · 次 / 消息 | 分轮 · 轮 / 运行 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · WriteSession · 固定长度写** | **2,383.04 · 2,221.83 – 2,601.96** | **19.064** | **0.342** | **1.500** | **3.475** | **6.66×** | **0** | **12** |
| ReaderWriterQueue · 成员初始化复制 | 813.31 · 706.46 – 880.25 | 6.506 | 1.300 | 2.225 | 8.167 | 3.25× | 0 | 12 |
| rigtorp/SPSCQueue · 成员复制＋目标写预取 | 323.04 · 310.61 – 331.72 | 2.584 | 2.917 | 5.633 | 12.125 | 1.00× | 0 | 12 |
| atomic_queue · 成员初始化复制 | 189.29 · 182.22 – 198.91 | 1.514 | 5.192 | 10.875 | 23.750 | 1.13× | 0 | 12 |

**16 B 回调循环 0 次 · 每次 24 轮 · 10,000,000 条**

| 实现 | 吞吐 · M条/s · 下方为范围 | 有效负载 · GB/s | p50 · ns / 次 | p99 · ns / 次 | p999 · ns / 次 | 回调区间 / 写入 · 墙钟时间比 | 回调循环 · 次 / 消息 | 分轮 · 轮 / 运行 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · WriteSession · 固定长度写** | **1,927.70 · 1,212.56 – 2,560.19** | **30.843** | **0.342** | **1.750** | **4.258** | **6.56×** | **0** | **24** |
| ReaderWriterQueue · 先清零，再 memcpy | 681.12 · 546.32 – 743.22 | 10.898 | 1.317 | 3.192 | 8.675 | 2.91× | 0 | 24 |
| rigtorp/SPSCQueue · 成员复制＋目标写预取 | 317.50 · 301.27 – 326.97 | 5.080 | 3.017 | 6.033 | 11.275 | 1.00× | 0 | 24 |
| atomic_queue · 成员初始化复制 | 126.00 · 111.58 – 136.03 | 2.016 | 6.008 | 24.950 | 38.900 | 1.01× | 0 | 24 |

**32 B 回调循环 0 次 · 每次 48 轮 · 10,000,000 条**

| 实现 | 吞吐 · M条/s · 下方为范围 | 有效负载 · GB/s | p50 · ns / 次 | p99 · ns / 次 | p999 · ns / 次 | 回调区间 / 写入 · 墙钟时间比 | 回调循环 · 次 / 消息 | 分轮 · 轮 / 运行 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · WriteSession · 固定长度写** | **1,165.02 · 760.92 – 1,417.00** | **37.281** | **0.817** | **3.567** | **8.900** | **3.72×** | **0** | **48** |
| ReaderWriterQueue · 先清零，再 memcpy | 498.11 · 449.77 – 526.76 | 15.940 | 1.883 | 11.542 | 30.042 | 2.14× | 0 | 48 |
| rigtorp/SPSCQueue · 成员复制＋目标写预取 | 258.44 · 230.39 – 280.96 | 8.270 | 3.375 | 9.033 | 15.842 | 1.00× | 0 | 48 |
| atomic_queue · 成员初始化复制 | 86.24 · 72.74 – 91.87 | 2.760 | 9.033 | 32.392 | 62.917 | 1.01× | 0 | 48 |

**64 B 回调循环 0 次 · 每次 96 轮 · 10,000,000 条**

| 实现 | 吞吐 · M条/s · 下方为范围 | 有效负载 · GB/s | p50 · ns / 次 | p99 · ns / 次 | p999 · ns / 次 | 回调区间 / 写入 · 墙钟时间比 | 回调循环 · 次 / 消息 | 分轮 · 轮 / 运行 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · WriteSession · 固定长度写** | **590.62 · 493.33 – 741.36** | **37.800** | **1.275** | **4.150** | **8.675** | **2.02×** | **0** | **96** |
| ReaderWriterQueue · 成员初始化复制 | 242.06 · 198.68 – 293.62 | 15.492 | 3.525 | 9.375 | 21.233 | 1.16× | 0 | 96 |
| rigtorp/SPSCQueue · 成员复制＋目标写预取 | 161.12 · 140.49 – 181.68 | 10.311 | 5.458 | 11.967 | 19.875 | 1.00× | 0 | 96 |
| atomic_queue · 成员初始化复制 | 69.86 · 65.94 – 73.53 | 4.471 | 14.783 | 42.458 | 76.708 | 1.01× | 0 | 96 |

p50 / p99 / p999 是“每组 1000 次成功写入的平均耗时”的分位数，不是单条消息端到端延时，也不是单次 write 的分位数。吞吐使用独立的未逐组计时运行。GB/s 按业务净负载计算。时间比 = 各轮首条回调开始至末条回调结束的耗时之和 ÷ 各轮写线程发送耗时之和，再对运行取平均；它包含分发、等待数据和调度，不是每次回调函数 CPU 耗时之和。

## 回调时间与背压

与吞吐表使用同一批正式运行。时间、次数按运行取平均；峰值 RSS 为这组运行的最大值。

**8 B 正式吞吐运行的逐次平均；RSS 为本组最大值**

| 实现 | 写线程 · ms | 回调区间 · ms | 受压消息 · 条 / 次运行 | 失败尝试 · 次 / 运行 | 重试时间 · ms | Writer 压力 · 内部事件 | 复制层拒绝 · 内部事件 | 峰值 RSS · MiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue** | **4.22** | **27.95** | **0.0** | **0.0** | **0.000** | **0** | **0** | **23.3** |
| ReaderWriterQueue | 12.36 | 39.96 | 0.0 | 0.0 | 0.000 | — | — | 17.2 |
| rigtorp/SPSCQueue | 30.97 | 31.03 | 0.0 | 0.0 | 0.000 | — | — | 16.1 |
| atomic_queue | 52.87 | 59.53 | 0.0 | 0.0 | 0.000 | — | — | 17.1 |

**16 B 正式吞吐运行的逐次平均；RSS 为本组最大值**

| 实现 | 写线程 · ms | 回调区间 · ms | 受压消息 · 条 / 次运行 | 失败尝试 · 次 / 运行 | 重试时间 · ms | Writer 压力 · 内部事件 | 复制层拒绝 · 内部事件 | 峰值 RSS · MiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue** | **5.57** | **34.40** | **0.0** | **0.0** | **0.000** | **0** | **0** | **24.1** |
| ReaderWriterQueue | 14.86 | 42.96 | 0.0 | 0.0 | 0.000 | — | — | 16.7 |
| rigtorp/SPSCQueue | 31.52 | 31.63 | 0.0 | 0.0 | 0.000 | — | — | 16.1 |
| atomic_queue | 79.70 | 80.86 | 0.0 | 0.0 | 0.000 | — | — | 16.6 |

**32 B 正式吞吐运行的逐次平均；RSS 为本组最大值**

| 实现 | 写线程 · ms | 回调区间 · ms | 受压消息 · 条 / 次运行 | 失败尝试 · 次 / 运行 | 重试时间 · ms | Writer 压力 · 内部事件 | 复制层拒绝 · 内部事件 | 峰值 RSS · MiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue** | **9.03** | **32.16** | **0.0** | **0.0** | **0.000** | **0** | **0** | **24.3** |
| ReaderWriterQueue | 20.14 | 42.96 | 0.0 | 0.0 | 0.000 | — | — | 16.5 |
| rigtorp/SPSCQueue | 38.87 | 38.96 | 0.0 | 0.0 | 0.000 | — | — | 16.1 |
| atomic_queue | 116.79 | 117.49 | 0.0 | 0.0 | 0.000 | — | — | 16.4 |

**64 B 正式吞吐运行的逐次平均；RSS 为本组最大值**

| 实现 | 写线程 · ms | 回调区间 · ms | 受压消息 · 条 / 次运行 | 失败尝试 · 次 / 运行 | 重试时间 · ms | Writer 压力 · 内部事件 | 复制层拒绝 · 内部事件 | 峰值 RSS · MiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue** | **17.29** | **34.34** | **0.0** | **0.0** | **0.000** | **0** | **0** | **24.4** |
| ReaderWriterQueue | 42.27 | 48.68 | 0.0 | 0.0 | 0.000 | — | — | 20.2 |
| rigtorp/SPSCQueue | 62.72 | 62.71 | 0.0 | 0.0 | 0.000 | — | — | 16.2 |
| atomic_queue | 143.31 | 145.31 | 0.0 | 0.0 | 0.000 | — | — | 16.1 |

受压消息至少经历一次 try 写入失败；失败尝试含重复重试，最终仍全部写入。Writer 与复制层内部事件仅 TwoBufQueue 提供，其他实现显示“—”，不能把“—”解释为 0。回调区间含分发、轮内等待及调度，时间比大于 1 本身不等于写端背压。RSS 包含进程开销，不等于缓存容量。

## 逐次测量值

展开查看全部 5 次吞吐及分位数。没有剔除异常值，也没有将多个运行的样本合并计算分位数。

### 8 B 5 次原始值 · 回调循环 0 次



| 实现 / 指标 | 第 1 次 | 第 2 次 | 第 3 次 | 第 4 次 | 第 5 次 | 平均 |
| --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 吞吐 M条/s** | **2,248.392** | **2,221.831** | **2,587.907** | **2,601.964** | **2,255.131** | **2,383.045** |
| **TwoBufQueue · p50 ns** | **0.333** | **0.375** | **0.333** | **0.333** | **0.333** | **0.342** |
| **TwoBufQueue · p99 ns** | **1.208** | **2.000** | **1.208** | **1.750** | **1.333** | **1.500** |
| **TwoBufQueue · p999 ns** | **1.875** | **7.542** | **1.958** | **3.625** | **2.375** | **3.475** |
| ReaderWriterQueue · 吞吐 M条/s | 813.537 | 837.983 | 880.249 | 828.306 | 706.462 | 813.308 |
| ReaderWriterQueue · p50 ns | 1.292 | 1.292 | 1.292 | 1.292 | 1.333 | 1.300 |
| ReaderWriterQueue · p99 ns | 1.625 | 2.500 | 2.083 | 2.167 | 2.750 | 2.225 |
| ReaderWriterQueue · p999 ns | 3.083 | 5.125 | 7.542 | 7.542 | 17.542 | 8.167 |
| rigtorp/SPSCQueue · 吞吐 M条/s | 310.605 | 323.165 | 323.008 | 326.683 | 331.715 | 323.035 |
| rigtorp/SPSCQueue · p50 ns | 2.958 | 2.917 | 2.917 | 2.917 | 2.875 | 2.917 |
| rigtorp/SPSCQueue · p99 ns | 6.917 | 6.917 | 4.250 | 5.750 | 4.333 | 5.633 |
| rigtorp/SPSCQueue · p999 ns | 16.333 | 17.167 | 7.875 | 11.500 | 7.750 | 12.125 |
| atomic_queue · 吞吐 M条/s | 182.219 | 189.311 | 186.854 | 189.147 | 198.908 | 189.288 |
| atomic_queue · p50 ns | 5.292 | 4.875 | 5.208 | 5.292 | 5.292 | 5.192 |
| atomic_queue · p99 ns | 17.625 | 8.000 | 8.375 | 7.958 | 12.417 | 10.875 |
| atomic_queue · p999 ns | 30.750 | 20.333 | 21.833 | 14.875 | 30.958 | 23.750 |

### 16 B 5 次原始值 · 回调循环 0 次



| 实现 / 指标 | 第 1 次 | 第 2 次 | 第 3 次 | 第 4 次 | 第 5 次 | 平均 |
| --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 吞吐 M条/s** | **1,613.868** | **1,212.562** | **2,332.611** | **1,919.248** | **2,560.193** | **1,927.696** |
| **TwoBufQueue · p50 ns** | **0.333** | **0.333** | **0.333** | **0.333** | **0.375** | **0.342** |
| **TwoBufQueue · p99 ns** | **1.542** | **1.167** | **3.000** | **1.667** | **1.375** | **1.750** |
| **TwoBufQueue · p999 ns** | **3.208** | **2.458** | **6.333** | **4.375** | **4.917** | **4.258** |
| ReaderWriterQueue · 吞吐 M条/s | 720.504 | 546.319 | 743.216 | 712.420 | 683.140 | 681.120 |
| ReaderWriterQueue · p50 ns | 1.375 | 1.333 | 1.292 | 1.292 | 1.292 | 1.317 |
| ReaderWriterQueue · p99 ns | 3.375 | 5.542 | 2.208 | 2.458 | 2.375 | 3.192 |
| ReaderWriterQueue · p999 ns | 16.458 | 13.083 | 5.375 | 4.333 | 4.125 | 8.675 |
| rigtorp/SPSCQueue · 吞吐 M条/s | 322.663 | 319.387 | 326.974 | 317.205 | 301.269 | 317.500 |
| rigtorp/SPSCQueue · p50 ns | 3.000 | 3.083 | 3.000 | 3.000 | 3.000 | 3.017 |
| rigtorp/SPSCQueue · p99 ns | 4.583 | 9.708 | 4.583 | 6.625 | 4.667 | 6.033 |
| rigtorp/SPSCQueue · p999 ns | 7.833 | 24.708 | 6.083 | 10.292 | 7.458 | 11.275 |
| atomic_queue · 吞吐 M条/s | 127.532 | 111.578 | 128.517 | 126.325 | 136.028 | 125.996 |
| atomic_queue · p50 ns | 5.708 | 6.083 | 6.167 | 6.042 | 6.042 | 6.008 |
| atomic_queue · p99 ns | 24.542 | 25.500 | 24.958 | 25.708 | 24.042 | 24.950 |
| atomic_queue · p999 ns | 33.542 | 52.958 | 31.833 | 32.917 | 43.250 | 38.900 |

### 32 B 5 次原始值 · 回调循环 0 次



| 实现 / 指标 | 第 1 次 | 第 2 次 | 第 3 次 | 第 4 次 | 第 5 次 | 平均 |
| --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 吞吐 M条/s** | **1,417.000** | **760.924** | **1,171.755** | **1,386.353** | **1,089.082** | **1,165.023** |
| **TwoBufQueue · p50 ns** | **0.917** | **0.958** | **0.583** | **0.750** | **0.875** | **0.817** |
| **TwoBufQueue · p99 ns** | **4.125** | **4.458** | **2.125** | **3.042** | **4.083** | **3.567** |
| **TwoBufQueue · p999 ns** | **9.917** | **13.292** | **4.917** | **7.042** | **9.333** | **8.900** |
| ReaderWriterQueue · 吞吐 M条/s | 522.067 | 494.157 | 449.765 | 526.759 | 497.808 | 498.111 |
| ReaderWriterQueue · p50 ns | 1.708 | 2.375 | 2.042 | 1.667 | 1.625 | 1.883 |
| ReaderWriterQueue · p99 ns | 10.917 | 17.000 | 9.375 | 10.083 | 10.333 | 11.542 |
| ReaderWriterQueue · p999 ns | 11.333 | 99.500 | 17.625 | 11.042 | 10.708 | 30.042 |
| rigtorp/SPSCQueue · 吞吐 M条/s | 253.519 | 280.961 | 256.831 | 230.386 | 270.501 | 258.440 |
| rigtorp/SPSCQueue · p50 ns | 3.500 | 3.250 | 3.292 | 3.542 | 3.292 | 3.375 |
| rigtorp/SPSCQueue · p99 ns | 13.708 | 5.583 | 6.458 | 12.625 | 6.792 | 9.033 |
| rigtorp/SPSCQueue · p999 ns | 27.458 | 10.708 | 10.667 | 19.333 | 11.042 | 15.842 |
| atomic_queue · 吞吐 M条/s | 89.726 | 91.869 | 89.067 | 72.740 | 87.781 | 86.237 |
| atomic_queue · p50 ns | 9.167 | 9.875 | 8.875 | 8.250 | 9.000 | 9.033 |
| atomic_queue · p99 ns | 33.417 | 45.625 | 27.708 | 25.583 | 29.625 | 32.392 |
| atomic_queue · p999 ns | 72.292 | 83.583 | 54.167 | 32.000 | 72.542 | 62.917 |

### 64 B 5 次原始值 · 回调循环 0 次



| 实现 / 指标 | 第 1 次 | 第 2 次 | 第 3 次 | 第 4 次 | 第 5 次 | 平均 |
| --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 吞吐 M条/s** | **630.729** | **493.326** | **522.749** | **564.932** | **741.358** | **590.619** |
| **TwoBufQueue · p50 ns** | **1.167** | **1.167** | **1.500** | **1.208** | **1.333** | **1.275** |
| **TwoBufQueue · p99 ns** | **3.125** | **3.000** | **5.167** | **4.125** | **5.333** | **4.150** |
| **TwoBufQueue · p999 ns** | **5.958** | **5.458** | **13.625** | **7.542** | **10.792** | **8.675** |
| ReaderWriterQueue · 吞吐 M条/s | 205.331 | 198.684 | 270.353 | 242.311 | 293.620 | 242.060 |
| ReaderWriterQueue · p50 ns | 2.958 | 4.083 | 3.833 | 3.792 | 2.958 | 3.525 |
| ReaderWriterQueue · p99 ns | 7.042 | 13.500 | 12.000 | 8.417 | 5.917 | 9.375 |
| ReaderWriterQueue · p999 ns | 10.917 | 39.167 | 20.958 | 25.708 | 9.417 | 21.233 |
| rigtorp/SPSCQueue · 吞吐 M条/s | 174.238 | 165.285 | 143.890 | 140.489 | 181.681 | 161.116 |
| rigtorp/SPSCQueue · p50 ns | 5.458 | 5.583 | 5.542 | 5.708 | 5.000 | 5.458 |
| rigtorp/SPSCQueue · p99 ns | 10.125 | 13.333 | 12.125 | 12.667 | 11.583 | 11.967 |
| rigtorp/SPSCQueue · p999 ns | 16.042 | 18.958 | 19.000 | 22.500 | 22.875 | 19.875 |
| atomic_queue · 吞吐 M条/s | 69.330 | 70.709 | 65.943 | 73.530 | 69.812 | 69.865 |
| atomic_queue · p50 ns | 16.833 | 15.750 | 12.750 | 13.083 | 15.500 | 14.783 |
| atomic_queue · p99 ns | 84.333 | 34.542 | 29.750 | 31.500 | 32.167 | 42.458 |
| atomic_queue · p999 ns | 145.250 | 75.917 | 42.667 | 52.917 | 66.792 | 76.708 |

波动较大的吞吐组：16 B · TwoBufQueue：1,212.56–2,560.19 M条/s；16 B · ReaderWriterQueue：546.32–743.22 M条/s；32 B · TwoBufQueue：760.92–1,417.00 M条/s；32 B · atomic_queue：72.74–91.87 M条/s；64 B · TwoBufQueue：493.33–741.36 M条/s；64 B · ReaderWriterQueue：198.68–293.62 M条/s；64 B · rigtorp/SPSCQueue：140.49–181.68 M条/s。所有测量均保留，未删除低值。

## 测试口径与复现信息

固定本次参数进行对比；本报告没有执行全参数寻优，也不声称全局最优。

### 发送、复制与排空

TwoBufQueue：Writer → 复制线程 → 第二层缓存 → 独立回调线程，逐消息调用回调。

按长度使用冻结的发布测试参数；同缓存普通 write 与 Session 使用完全相同的块大小、池容量和读端参数。完整参数如下。四种实现正式写入次数与净负载长度相同。

每轮上限按缓存的 80% / 消息字节数计算。TwoBufQueue 不扫描当前块，publish_count 为 UINT32_MAX；停止发送计时后调用 finish_block 交接当前块，再等待本轮全部回调完成。预热也在计时外交接，最终尾部统一 finish。没有增加额外消息，原生 SPSC 每条已发布，轮末无需额外交接。

两份报告同时改变缓存大小与回调工作量，不能将两组差异全部归因于缓存；比较容量影响需另做相同回调负载的对照。

不逐条 flush · 轮末计时外交接；publish_count 固定为 4294967295。仅预触碰写端数据页，第二层数据页没有额外预触碰。

吞吐和分组延时均在计时前准备整轮输入；每条消息的全部字都随序号变化；四种实现使用同一完整负载；队列内仍复制一次完整负载；各行标明成员复制、目标写预取或先清零再 memcpy，完整参数记录实际选择；先清零的额外存储也包含在计时中。输入缓冲单独计算，不算队列容量，也不保证常驻 L1。

### 时钟、调度与限制

本机为 macOS-14.4.1-arm64-arm-64bit / arm64，Release 优化构建。

计时器：mach_absolute_time；每组 1000 次写入。组均值刻度约 0.0417 ns / 次。

写线程调度：高 QoS · 未硬绑核。调度打断和其他系统活动仍可能影响结果；复制与回调线程保留自身调度策略。

吞吐只累计写线程发送时间；排空在计时之外。并发复制 / 回调仍可能争用缓存和带宽，因此不是持续端到端吞吐。

**p999 仅供观察：** 每次延时测量约有 9,918–9,990 个组均值样本，少于框架建议的 100,000 个。p999 的尾部覆盖有限，不能据此做稳定的尾延时承诺。p999 表示 99.9% 分位数；平均值不代表置信区间。

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
    "is_scan_current": false,
    "reader_spin_count": 1000,
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
    "random_seed": 42,
    "timeout_seconds": 180,
    "buffer_budget_bytes": 16777216,
    "use_write_session": true,
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
    "is_scan_current": false,
    "reader_spin_count": 0,
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
    "use_write_session": true,
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
    "is_scan_current": false,
    "reader_spin_count": 0,
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
    "use_write_session": true,
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
    "is_scan_current": false,
    "reader_spin_count": 10000,
    "rescan_interval_ns": 30000000,
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
    "random_seed": 42,
    "timeout_seconds": 180,
    "buffer_budget_bytes": 16777216,
    "use_write_session": true,
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

生成于 2026-10-11T11:00:47.224397+08:00 · 共 160 次正式运行，16 次负载校验，0 次回调校准。

编译器：Clang 18.1.8

被测程序 SHA-256：a229326cc6898487c8603758696f23484e0a611e6a2f51e4b580528a0a3909c7

[reproduction.json](../../performance_data/release_20261011_inline_fixed_110024/8m_session/reproduction.json) · [results.json](../../performance_data/release_20261011_inline_fixed_110024/8m_session/results.json) · [raw.jsonl](../../performance_data/release_20261011_inline_fixed_110024/8m_session/raw.jsonl) · [profile.json](../../performance_data/release_20261011_inline_fixed_110024/8m_session/profile.json) · [environment.json](../../performance_data/release_20261011_inline_fixed_110024/8m_session/environment.json) · [audit.json](../../performance_data/release_20261011_inline_fixed_110024/8m_session/audit.json) · [dependencies.json](../../performance_data/release_20261011_inline_fixed_110024/8m_session/dependencies.json)

第三方原文件按 benchmarks/spsc/spsc_dependencies.json 校验，未修改队列实现；外部 Payload 的复制策略为 完整负载复制；rigtorp 可选目标写预取，ReaderWriterQueue 可选先清零再 memcpy，复制完整负载。逐次命令、配置、校验结果与内部计数保存在 raw.jsonl。

此 HTML 内嵌汇总数据，离线可查看、筛选与导出。原始日志链接需要同目录下的 performance_data 文件夹。
