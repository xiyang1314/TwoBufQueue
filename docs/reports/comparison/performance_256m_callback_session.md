TWOBUFQUEUE / PERFORMANCE NOTES

# 256 MiB 缓存 · 回调时间校准 · 单块 Session 双重循环

固定长度、不写长度前缀的 TwoBufQueue，与三种开源 SPSC 队列对比。TwoBufQueue 包含完整第二层复制与独立回调线程；第三方使用原生消费线程逐条回调。

10 百万条 / 次 5 次平均 / 指标 8 B / 16 B / 32 B / 64 B publish_count = 4294967295 不逐条 flush · 轮末计时外交接 高 QoS · 未硬绑核

[总览](#按消息长度查看吞吐) · [吞吐与延时](#吞吐与写入耗时) · [时间与背压](#回调时间与背压) · [原始值](#逐次测量值) · [测试口径](#测试口径与复现信息)

其他报告：[256M 普通](performance_256m_callback.md) · [8M 普通](performance_8m_zero.md) · [8M Session](performance_8m_zero_session.md)

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

逐长度以 TwoBufQueue 的吞吐测量为基准，以整轮回调区间达到写线程时间的 2～3 倍为校准目标，未达到时保留实际值；该长度的四个实现使用同一循环次数。其他实现写入速度不同，其时间比可以不在 2～3 倍内。

**逐长度校准结果：** 8 B：1 次循环，校准时间比 3.09×（目标未达到）；16 B：1 次循环，校准时间比 2.17×；32 B：2 次循环，校准时间比 1.95×（目标未达到）；64 B：4 次循环，校准时间比 2.38×。

本次是普通 write 与单块 Session 的同参数对照，没有重新寻优。吞吐和各延时指标使用同一构造配置。

同缓存的两份报告共享本次实测的开源对照数据；没有使用历史成绩。

每层数据池严格为 256 MiB；原生队列的保留槽与 rigtorp 首尾 padding 纳入这个 payload 存储预算，队列元数据和输入另计。

Session 正常遇到块满返回 insufficient_space，次数记入 session_block_exhaustions；这不是背压或丢消息。池/队列受压通过 Writer 内部压力统计报告。

rigtorp_write_prefetch 在完整参数中记录；开关在测试开始前选择模板，普通复制和带提示构造都只复制一次完整负载，不额外写零。atomic_queue 保持原有调用。

ReaderWriterQueue 16/32 B 使用 copy_after_zero：队列槽位默认清零后 memcpy 一次完整负载；8/64 B 保留成员复制。清零和复制均在写入计时内。各实现的调用端优化在行标签和完整参数中公开，不能把所有差异归因于队列算法。

get_current_session → 内层逐条写 → 空间不足退出 → 外层 get_next_session。获取、等待、换块与 Session 状态归还包含在写入计时中。分组延时跨块继续计时，没有在块边界提前结束计时组。

不主动 flush；flush 只发布位置，不切换 block。每轮尾部在计时外 finish_block，最后一轮 finish，关闭 current 扫描也能收齐尾部。

回调循环以普通 write 校准后固定，同一长度的 Session 和全部开源实现都使用相同次数；Session 写端变快时，实际回调/写入时间比可以超过 3。

本轮全部重新测量于提交 f698f25：只取消测试框架六个 lambda 的强制内联，Writer::write 的内联和库代码保持原样。8 B 回退的对照、历史源码与结论边界见 docs/performance_data/twobufqueue_8b_regression_20261011；本轮没有使用历史成绩替换。

本轮合计 400 次独立正式测量、40 次完整负载/FIFO 校验、45 次回调校准；同缓存的普通 write 与 Session 共享本轮开源数据，因此四份报告显示的运行次数不可直接相加。

本轮 8/16/32/64 B 的回调循环固定为 1/1/2/4 次，普通 write 校准时间比分别约 3.09/2.17/1.95/2.38；8 B 与 32 B 略超出 2～3 倍目标范围，保留实际结果，不再挑选测量。

消息长度

8 B 吞吐 · M条/s

TwoBufQueue 2,383.59

ReaderWriterQueue 817.50

rigtorp/SPSCQueue 301.97

atomic_queue 189.63

16 B 吞吐 · M条/s

TwoBufQueue 1,089.93

ReaderWriterQueue 572.91

rigtorp/SPSCQueue 293.66

atomic_queue 121.32

32 B 吞吐 · M条/s

TwoBufQueue 456.44

ReaderWriterQueue 397.57

rigtorp/SPSCQueue 182.68

atomic_queue 80.03

64 B 吞吐 · M条/s

TwoBufQueue 299.51

ReaderWriterQueue 104.86

rigtorp/SPSCQueue 122.45

atomic_queue 73.80

## 吞吐与写入耗时

每个指标先按单次运行计算，再取 5 次算术平均。TwoBufQueue 整行加粗并加边框。

**8 B 回调循环 1 次 · 每次 1 轮 · 10,000,000 条**

| 实现 | 吞吐 · M条/s · 下方为范围 | 有效负载 · GB/s | p50 · ns / 次 | p99 · ns / 次 | p999 · ns / 次 | 回调区间 / 写入 · 墙钟时间比 | 回调循环 · 次 / 消息 | 分轮 · 轮 / 运行 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · WriteSession · 固定长度写** | **2,383.59 · 1,961.22 – 2,772.10** | **19.069** | **0.375** | **2.042** | **4.325** | **6.84×** | **1** | **1** |
| ReaderWriterQueue · 成员初始化复制 | 817.50 · 783.84 – 858.57 | 6.540 | 1.300 | 2.192 | 3.733 | 3.28× | 1 | 1 |
| rigtorp/SPSCQueue · 成员复制＋目标写预取 | 301.97 · 286.81 – 325.82 | 2.416 | 3.075 | 5.917 | 10.967 | 1.00× | 1 | 1 |
| atomic_queue · 成员初始化复制 | 189.63 · 178.89 – 196.80 | 1.517 | 5.100 | 20.217 | 33.225 | 1.20× | 1 | 1 |

**16 B 回调循环 1 次 · 每次 1 轮 · 10,000,000 条**

| 实现 | 吞吐 · M条/s · 下方为范围 | 有效负载 · GB/s | p50 · ns / 次 | p99 · ns / 次 | p999 · ns / 次 | 回调区间 / 写入 · 墙钟时间比 | 回调循环 · 次 / 消息 | 分轮 · 轮 / 运行 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · WriteSession · 固定长度写** | **1,089.93 · 941.58 – 1,321.99** | **17.439** | **0.642** | **2.700** | **7.258** | **3.91×** | **1** | **1** |
| ReaderWriterQueue · 先清零，再 memcpy | 572.91 · 392.34 – 771.09 | 9.167 | 1.892 | 10.908 | 136.142 | 2.58× | 1 | 1 |
| rigtorp/SPSCQueue · 成员复制＋目标写预取 | 293.66 · 274.34 – 310.42 | 4.699 | 3.158 | 5.950 | 9.325 | 1.00× | 1 | 1 |
| atomic_queue · 成员初始化复制 | 121.32 · 103.68 – 128.55 | 1.941 | 6.092 | 16.667 | 29.067 | 1.00× | 1 | 1 |

**32 B 回调循环 2 次 · 每次 2 轮 · 10,000,000 条**

| 实现 | 吞吐 · M条/s · 下方为范围 | 有效负载 · GB/s | p50 · ns / 次 | p99 · ns / 次 | p999 · ns / 次 | 回调区间 / 写入 · 墙钟时间比 | 回调循环 · 次 / 消息 | 分轮 · 轮 / 运行 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · WriteSession · 固定长度写** | **456.44 · 166.47 – 658.49** | **14.606** | **1.633** | **6.400** | **15.842** | **2.46×** | **2** | **2** |
| ReaderWriterQueue · 先清零，再 memcpy | 397.57 · 330.33 – 508.46 | 12.722 | 2.508 | 9.308 | 39.692 | 2.17× | 2 | 2 |
| rigtorp/SPSCQueue · 成员复制＋目标写预取 | 182.68 · 150.68 – 210.64 | 5.846 | 4.575 | 14.192 | 79.408 | 1.00× | 2 | 2 |
| atomic_queue · 成员初始化复制 | 80.03 · 74.14 – 85.24 | 2.561 | 9.508 | 28.142 | 46.592 | 1.00× | 2 | 2 |

**64 B 回调循环 4 次 · 每次 3 轮 · 10,000,000 条**

| 实现 | 吞吐 · M条/s · 下方为范围 | 有效负载 · GB/s | p50 · ns / 次 | p99 · ns / 次 | p999 · ns / 次 | 回调区间 / 写入 · 墙钟时间比 | 回调循环 · 次 / 消息 | 分轮 · 轮 / 运行 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · WriteSession · 固定长度写** | **299.51 · 261.11 – 360.61** | **19.168** | **2.992** | **14.825** | **154.875** | **2.58×** | **4** | **3** |
| ReaderWriterQueue · 成员初始化复制 | 104.86 · 69.74 – 148.56 | 6.711 | 5.733 | 44.217 | 271.517 | 1.19× | 4 | 3 |
| rigtorp/SPSCQueue · 成员复制＋目标写预取 | 122.45 · 120.66 – 125.46 | 7.837 | 7.933 | 13.058 | 24.375 | 1.00× | 4 | 3 |
| atomic_queue · 成员初始化复制 | 73.80 · 70.04 – 80.20 | 4.723 | 12.083 | 37.467 | 207.942 | 1.00× | 4 | 3 |

p50 / p99 / p999 是“每组 1000 次成功写入的平均耗时”的分位数，不是单条消息端到端延时，也不是单次 write 的分位数。吞吐使用独立的未逐组计时运行。GB/s 按业务净负载计算。时间比 = 各轮首条回调开始至末条回调结束的耗时之和 ÷ 各轮写线程发送耗时之和，再对运行取平均；它包含分发、等待数据和调度，不是每次回调函数 CPU 耗时之和。

## 回调时间与背压

与吞吐表使用同一批正式运行。时间、次数按运行取平均；峰值 RSS 为这组运行的最大值。

**8 B 正式吞吐运行的逐次平均；RSS 为本组最大值**

| 实现 | 写线程 · ms | 回调区间 · ms | 受压消息 · 条 / 次运行 | 失败尝试 · 次 / 运行 | 重试时间 · ms | Writer 压力 · 内部事件 | 复制层拒绝 · 内部事件 | 峰值 RSS · MiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue** | **4.26** | **28.77** | **0.0** | **0.0** | **0.000** | **0** | **0** | **395.6** |
| ReaderWriterQueue | 12.25 | 40.11 | 0.0 | 0.0 | 0.000 | — | — | 367.2 |
| rigtorp/SPSCQueue | 33.19 | 33.19 | 0.0 | 0.0 | 0.000 | — | — | 333.9 |
| atomic_queue | 52.80 | 63.40 | 0.0 | 0.0 | 0.000 | — | — | 365.9 |

**16 B 正式吞吐运行的逐次平均；RSS 为本组最大值**

| 实现 | 写线程 · ms | 回调区间 · ms | 受压消息 · 条 / 次运行 | 失败尝试 · 次 / 运行 | 重试时间 · ms | Writer 压力 · 内部事件 | 复制层拒绝 · 内部事件 | 峰值 RSS · MiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue** | **9.31** | **36.05** | **0.0** | **0.0** | **0.000** | **0** | **0** | **544.8** |
| ReaderWriterQueue | 18.89 | 45.27 | 0.0 | 0.0 | 0.000 | — | — | 427.7 |
| rigtorp/SPSCQueue | 34.15 | 34.15 | 0.0 | 0.0 | 0.000 | — | — | 410.3 |
| atomic_queue | 82.95 | 83.15 | 0.0 | 0.0 | 0.000 | — | — | 426.3 |

**32 B 正式吞吐运行的逐次平均；RSS 为本组最大值**

| 实现 | 写线程 · ms | 回调区间 · ms | 受压消息 · 条 / 次运行 | 失败尝试 · 次 / 运行 | 重试时间 · ms | Writer 压力 · 内部事件 | 复制层拒绝 · 内部事件 | 峰值 RSS · MiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue** | **28.36** | **59.11** | **0.0** | **0.0** | **0.000** | **0** | **0** | **583.4** |
| ReaderWriterQueue | 25.89 | 55.02 | 0.0 | 0.0 | 0.000 | — | — | 471.9 |
| rigtorp/SPSCQueue | 55.73 | 55.73 | 0.0 | 0.0 | 0.000 | — | — | 462.5 |
| atomic_queue | 125.32 | 125.36 | 0.0 | 0.0 | 0.000 | — | — | 470.5 |

**64 B 正式吞吐运行的逐次平均；RSS 为本组最大值**

| 实现 | 写线程 · ms | 回调区间 · ms | 受压消息 · 条 / 次运行 | 失败尝试 · 次 / 运行 | 重试时间 · ms | Writer 压力 · 内部事件 | 复制层拒绝 · 内部事件 | 峰值 RSS · MiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue** | **33.86** | **86.35** | **0.0** | **0.0** | **0.000** | **0** | **0** | **591.5** |
| ReaderWriterQueue | 101.30 | 118.19 | 0.0 | 0.0 | 0.000 | — | — | 590.7 |
| rigtorp/SPSCQueue | 81.68 | 81.68 | 0.0 | 0.0 | 0.000 | — | — | 462.5 |
| atomic_queue | 135.86 | 136.07 | 0.0 | 0.0 | 0.000 | — | — | 466.6 |

受压消息至少经历一次 try 写入失败；失败尝试含重复重试，最终仍全部写入。Writer 与复制层内部事件仅 TwoBufQueue 提供，其他实现显示“—”，不能把“—”解释为 0。回调区间含分发、轮内等待及调度，时间比大于 1 本身不等于写端背压。RSS 包含进程开销，不等于缓存容量。

## 逐次测量值

展开查看全部 5 次吞吐及分位数。没有剔除异常值，也没有将多个运行的样本合并计算分位数。

### 8 B 5 次原始值 · 回调循环 1 次



| 实现 / 指标 | 第 1 次 | 第 2 次 | 第 3 次 | 第 4 次 | 第 5 次 | 平均 |
| --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 吞吐 M条/s** | **2,647.838** | **1,961.217** | **2,772.099** | **2,157.982** | **2,378.805** | **2,383.588** |
| **TwoBufQueue · p50 ns** | **0.583** | **0.375** | **0.333** | **0.292** | **0.292** | **0.375** |
| **TwoBufQueue · p99 ns** | **3.667** | **2.083** | **1.708** | **1.417** | **1.333** | **2.042** |
| **TwoBufQueue · p999 ns** | **11.000** | **3.750** | **2.833** | **2.250** | **1.792** | **4.325** |
| ReaderWriterQueue · 吞吐 M条/s | 783.845 | 840.142 | 858.566 | 797.955 | 806.978 | 817.497 |
| ReaderWriterQueue · p50 ns | 1.333 | 1.292 | 1.292 | 1.292 | 1.292 | 1.300 |
| ReaderWriterQueue · p99 ns | 3.083 | 1.875 | 1.917 | 1.833 | 2.250 | 2.192 |
| ReaderWriterQueue · p999 ns | 7.375 | 2.208 | 2.333 | 2.250 | 4.500 | 3.733 |
| rigtorp/SPSCQueue · 吞吐 M条/s | 325.818 | 294.533 | 290.055 | 312.630 | 286.807 | 301.969 |
| rigtorp/SPSCQueue · p50 ns | 3.250 | 3.083 | 3.042 | 2.958 | 3.042 | 3.075 |
| rigtorp/SPSCQueue · p99 ns | 7.500 | 4.833 | 4.667 | 5.958 | 6.625 | 5.917 |
| rigtorp/SPSCQueue · p999 ns | 12.208 | 8.625 | 9.125 | 11.250 | 13.625 | 10.967 |
| atomic_queue · 吞吐 M条/s | 196.798 | 190.486 | 186.917 | 178.889 | 195.059 | 189.630 |
| atomic_queue · p50 ns | 5.458 | 5.208 | 5.042 | 4.875 | 4.917 | 5.100 |
| atomic_queue · p99 ns | 10.375 | 7.375 | 36.792 | 36.250 | 10.292 | 20.217 |
| atomic_queue · p999 ns | 33.917 | 18.125 | 39.333 | 38.208 | 36.542 | 33.225 |

### 16 B 5 次原始值 · 回调循环 1 次



| 实现 / 指标 | 第 1 次 | 第 2 次 | 第 3 次 | 第 4 次 | 第 5 次 | 平均 |
| --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 吞吐 M条/s** | **998.677** | **1,041.237** | **941.575** | **1,146.154** | **1,321.986** | **1,089.926** |
| **TwoBufQueue · p50 ns** | **0.542** | **0.750** | **0.708** | **0.583** | **0.625** | **0.642** |
| **TwoBufQueue · p99 ns** | **1.208** | **3.375** | **2.792** | **3.917** | **2.208** | **2.700** |
| **TwoBufQueue · p999 ns** | **1.542** | **8.375** | **9.542** | **8.167** | **8.667** | **7.258** |
| ReaderWriterQueue · 吞吐 M条/s | 393.225 | 660.139 | 392.340 | 647.766 | 771.092 | 572.912 |
| ReaderWriterQueue · p50 ns | 2.458 | 1.292 | 1.250 | 1.375 | 3.083 | 1.892 |
| ReaderWriterQueue · p99 ns | 27.375 | 2.542 | 2.292 | 2.750 | 19.583 | 10.908 |
| ReaderWriterQueue · p999 ns | 354.958 | 5.833 | 3.083 | 8.708 | 308.125 | 136.142 |
| rigtorp/SPSCQueue · 吞吐 M条/s | 306.434 | 274.880 | 302.219 | 274.338 | 310.421 | 293.658 |
| rigtorp/SPSCQueue · p50 ns | 3.125 | 3.167 | 3.125 | 3.167 | 3.208 | 3.158 |
| rigtorp/SPSCQueue · p99 ns | 4.917 | 8.417 | 5.042 | 5.500 | 5.875 | 5.950 |
| rigtorp/SPSCQueue · p999 ns | 8.167 | 11.292 | 9.208 | 8.375 | 9.583 | 9.325 |
| atomic_queue · 吞吐 M条/s | 125.365 | 121.402 | 103.676 | 128.547 | 127.603 | 121.319 |
| atomic_queue · p50 ns | 5.875 | 6.083 | 6.000 | 6.125 | 6.375 | 6.092 |
| atomic_queue · p99 ns | 16.042 | 15.000 | 14.292 | 15.542 | 22.458 | 16.667 |
| atomic_queue · p999 ns | 21.958 | 21.708 | 16.500 | 34.667 | 50.500 | 29.067 |

### 32 B 5 次原始值 · 回调循环 2 次



| 实现 / 指标 | 第 1 次 | 第 2 次 | 第 3 次 | 第 4 次 | 第 5 次 | 平均 |
| --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 吞吐 M条/s** | **327.027** | **636.895** | **166.473** | **658.487** | **493.308** | **456.438** |
| **TwoBufQueue · p50 ns** | **1.833** | **1.708** | **1.625** | **1.542** | **1.458** | **1.633** |
| **TwoBufQueue · p99 ns** | **6.583** | **5.500** | **10.375** | **5.292** | **4.250** | **6.400** |
| **TwoBufQueue · p999 ns** | **17.292** | **19.292** | **17.958** | **13.917** | **10.750** | **15.842** |
| ReaderWriterQueue · 吞吐 M条/s | 508.457 | 349.990 | 330.329 | 346.929 | 452.140 | 397.569 |
| ReaderWriterQueue · p50 ns | 1.917 | 3.167 | 1.958 | 2.125 | 3.375 | 2.508 |
| ReaderWriterQueue · p99 ns | 4.292 | 12.000 | 6.667 | 7.542 | 16.042 | 9.308 |
| ReaderWriterQueue · p999 ns | 7.375 | 18.458 | 11.833 | 22.333 | 138.458 | 39.692 |
| rigtorp/SPSCQueue · 吞吐 M条/s | 210.642 | 160.458 | 150.679 | 207.355 | 184.267 | 182.680 |
| rigtorp/SPSCQueue · p50 ns | 4.125 | 4.125 | 4.458 | 5.917 | 4.250 | 4.575 |
| rigtorp/SPSCQueue · p99 ns | 6.167 | 6.292 | 13.208 | 32.917 | 12.375 | 14.192 |
| rigtorp/SPSCQueue · p999 ns | 8.083 | 11.375 | 18.542 | 341.625 | 17.417 | 79.408 |
| atomic_queue · 吞吐 M条/s | 84.329 | 85.241 | 80.072 | 74.142 | 76.375 | 80.032 |
| atomic_queue · p50 ns | 8.708 | 9.750 | 9.083 | 10.917 | 9.083 | 9.508 |
| atomic_queue · p99 ns | 26.583 | 27.792 | 28.667 | 30.708 | 26.958 | 28.142 |
| atomic_queue · p999 ns | 33.875 | 58.958 | 54.333 | 48.917 | 36.875 | 46.592 |

### 64 B 5 次原始值 · 回调循环 4 次



| 实现 / 指标 | 第 1 次 | 第 2 次 | 第 3 次 | 第 4 次 | 第 5 次 | 平均 |
| --- | --- | --- | --- | --- | --- | --- |
| **TwoBufQueue · 吞吐 M条/s** | **264.775** | **299.385** | **261.113** | **360.615** | **311.638** | **299.505** |
| **TwoBufQueue · p50 ns** | **2.958** | **2.583** | **3.375** | **3.333** | **2.708** | **2.992** |
| **TwoBufQueue · p99 ns** | **12.875** | **8.750** | **23.875** | **15.875** | **12.750** | **14.825** |
| **TwoBufQueue · p999 ns** | **25.208** | **17.958** | **683.708** | **27.125** | **20.375** | **154.875** |
| ReaderWriterQueue · 吞吐 M条/s | 95.810 | 69.739 | 113.291 | 148.562 | 96.918 | 104.864 |
| ReaderWriterQueue · p50 ns | 4.417 | 6.667 | 5.208 | 4.917 | 7.458 | 5.733 |
| ReaderWriterQueue · p99 ns | 9.625 | 115.250 | 21.042 | 14.708 | 60.458 | 44.217 |
| ReaderWriterQueue · p999 ns | 14.500 | 772.333 | 38.208 | 37.417 | 495.125 | 271.517 |
| rigtorp/SPSCQueue · 吞吐 M条/s | 120.663 | 121.452 | 121.275 | 125.464 | 123.387 | 122.449 |
| rigtorp/SPSCQueue · p50 ns | 7.875 | 8.000 | 8.000 | 7.958 | 7.833 | 7.933 |
| rigtorp/SPSCQueue · p99 ns | 11.333 | 13.500 | 12.708 | 15.833 | 11.917 | 13.058 |
| rigtorp/SPSCQueue · p999 ns | 17.125 | 21.375 | 31.083 | 34.667 | 17.625 | 24.375 |
| atomic_queue · 吞吐 M条/s | 71.002 | 76.260 | 71.506 | 80.201 | 70.036 | 73.801 |
| atomic_queue · p50 ns | 13.042 | 11.958 | 11.208 | 11.750 | 12.458 | 12.083 |
| atomic_queue · p99 ns | 29.083 | 32.833 | 26.125 | 27.417 | 71.875 | 37.467 |
| atomic_queue · p999 ns | 58.583 | 73.250 | 56.208 | 62.125 | 789.542 | 207.942 |

波动较大的吞吐组：8 B · TwoBufQueue：1,961.22–2,772.10 M条/s；16 B · TwoBufQueue：941.58–1,321.99 M条/s；16 B · ReaderWriterQueue：392.34–771.09 M条/s；32 B · TwoBufQueue：166.47–658.49 M条/s；32 B · ReaderWriterQueue：330.33–508.46 M条/s；32 B · rigtorp/SPSCQueue：150.68–210.64 M条/s；64 B · TwoBufQueue：261.11–360.61 M条/s；64 B · ReaderWriterQueue：69.74–148.56 M条/s。所有测量均保留，未删除低值。

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

**p999 仅供观察：** 每次延时测量约有 9,999–10,000 个组均值样本，少于框架建议的 100,000 个。p999 的尾部覆盖有限，不能据此做稳定的尾延时承诺。p999 表示 99.9% 分位数；平均值不代表置信区间。

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
    "is_scan_current": false,
    "reader_spin_count": 10000,
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
    "is_scan_current": false,
    "reader_spin_count": 10000,
    "rescan_interval_ns": 30000000,
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

生成于 2026-10-11T11:00:47.224397+08:00 · 共 160 次正式运行，16 次负载校验，45 次回调校准。

编译器：Clang 18.1.8

被测程序 SHA-256：a229326cc6898487c8603758696f23484e0a611e6a2f51e4b580528a0a3909c7

[reproduction.json](../../performance_data/release_20261011_inline_fixed_110024/256m_session/reproduction.json) · [results.json](../../performance_data/release_20261011_inline_fixed_110024/256m_session/results.json) · [raw.jsonl](../../performance_data/release_20261011_inline_fixed_110024/256m_session/raw.jsonl) · [profile.json](../../performance_data/release_20261011_inline_fixed_110024/256m_session/profile.json) · [environment.json](../../performance_data/release_20261011_inline_fixed_110024/256m_session/environment.json) · [audit.json](../../performance_data/release_20261011_inline_fixed_110024/256m_session/audit.json) · [dependencies.json](../../performance_data/release_20261011_inline_fixed_110024/256m_session/dependencies.json) · [callback-calibration.json](../../performance_data/release_20261011_inline_fixed_110024/256m_session/callback-calibration.json)

第三方原文件按 benchmarks/spsc/spsc_dependencies.json 校验，未修改队列实现；外部 Payload 的复制策略为 完整负载复制；rigtorp 可选目标写预取，ReaderWriterQueue 可选先清零再 memcpy，复制完整负载。逐次命令、配置、校验结果与内部计数保存在 raw.jsonl。

此 HTML 内嵌汇总数据，离线可查看、筛选与导出。原始日志链接需要同目录下的 performance_data 文件夹。
