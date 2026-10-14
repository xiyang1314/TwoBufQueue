# 24、48、96 KiB：哪个业务场景更能体现优势？

全部队列使用相同工作集。TwoBufQueue第一层总池固定16 KiB，第二层256 MiB；开源队列使用常规入队方式及256 MiB槽位存储。

本次选择：48 KiB业务工作集

## 怎样选最优

综合1、2、4、8、16 MiB发送量和三次独立运行。每个条件重复8次“发送消息 → 执行64次业务”，取这8个业务窗口耗时的中位数，减少偶发停顿的影响。

同条件下，用 **最快开源对照的业务耗时 ÷ TwoBufQueue的业务耗时** 得到优势倍数，再综合15个条件取几何平均。大于1表示TwoBufQueue更快；要求多数条件占优、至少两次独立运行综合占优。选得分最高的共同业务工作集。

| 业务工作集 | 综合优势倍数 ↑ | 占优条件数 | 三次独立优势倍数 |
| --- | --- | --- | --- |
| 24 KiB | 1.428× | 9 / 15 | 1.443× / 1.568× / 1.287× |
| 48 KiB | 1.796× | 15 / 15 | 1.903× / 1.665× / 1.830× |
| 96 KiB | 1.788× | 13 / 15 | 1.730× / 1.548× / 2.134× |

这里选择的是展示用的业务场景。业务表大小是负载参数，全部队列共同采用所选大小；没有给不同队列设置不同业务负载。倍数来自重复业务窗口的中位数比较，不能当作p50、p99或p999。

## 全部参数的业务耗时

“前”为发消息前16次业务的平均耗时；“后”为发消息后紧接着64次业务的平均耗时。每次业务访问并更新8个节点。数值是三次独立运行、每次8个窗口的均值。

| 工作集 | 发送量 | queue | 前均值 ns/次 | 后均值 ns/次 |
| --- | --- | --- | --- | --- |
| 24 KiB | 1 MiB | TwoBufQueue | 14.8 | 20.1 |
| 24 KiB | 1 MiB | ReaderWriterQueue | 14.3 | 19.0 |
| 24 KiB | 1 MiB | rigtorp::SPSCQueue | 14.9 | 16.6 |
| 24 KiB | 1 MiB | atomic_queue | 14.9 | 27.6 |
| 24 KiB | 2 MiB | TwoBufQueue | 14.3 | 19.3 |
| 24 KiB | 2 MiB | ReaderWriterQueue | 13.9 | 19.4 |
| 24 KiB | 2 MiB | rigtorp::SPSCQueue | 13.7 | 19.6 |
| 24 KiB | 2 MiB | atomic_queue | 14.4 | 32.9 |
| 24 KiB | 4 MiB | TwoBufQueue | 14.4 | 24.1 |
| 24 KiB | 4 MiB | ReaderWriterQueue | 14.0 | 36.7 |
| 24 KiB | 4 MiB | rigtorp::SPSCQueue | 13.1 | 32.0 |
| 24 KiB | 4 MiB | atomic_queue | 13.8 | 36.9 |
| 24 KiB | 8 MiB | TwoBufQueue | 13.9 | 26.1 |
| 24 KiB | 8 MiB | ReaderWriterQueue | 13.5 | 49.6 |
| 24 KiB | 8 MiB | rigtorp::SPSCQueue | 13.3 | 48.4 |
| 24 KiB | 8 MiB | atomic_queue | 14.1 | 53.5 |
| 24 KiB | 16 MiB | TwoBufQueue | 14.0 | 29.4 |
| 24 KiB | 16 MiB | ReaderWriterQueue | 14.1 | 60.5 |
| 24 KiB | 16 MiB | rigtorp::SPSCQueue | 13.5 | 80.5 |
| 24 KiB | 16 MiB | atomic_queue | 15.3 | 72.3 |
| 48 KiB | 1 MiB | TwoBufQueue | 13.9 | 15.5 |
| 48 KiB | 1 MiB | ReaderWriterQueue | 13.9 | 28.0 |
| 48 KiB | 1 MiB | rigtorp::SPSCQueue | 13.6 | 16.7 |
| 48 KiB | 1 MiB | atomic_queue | 15.3 | 39.7 |
| 48 KiB | 2 MiB | TwoBufQueue | 13.9 | 21.1 |
| 48 KiB | 2 MiB | ReaderWriterQueue | 13.8 | 33.4 |
| 48 KiB | 2 MiB | rigtorp::SPSCQueue | 13.5 | 23.9 |
| 48 KiB | 2 MiB | atomic_queue | 13.6 | 42.5 |
| 48 KiB | 4 MiB | TwoBufQueue | 14.2 | 32.3 |
| 48 KiB | 4 MiB | ReaderWriterQueue | 14.0 | 55.7 |
| 48 KiB | 4 MiB | rigtorp::SPSCQueue | 13.1 | 34.2 |
| 48 KiB | 4 MiB | atomic_queue | 14.0 | 54.2 |
| 48 KiB | 8 MiB | TwoBufQueue | 14.2 | 26.3 |
| 48 KiB | 8 MiB | ReaderWriterQueue | 14.3 | 165.4 |
| 48 KiB | 8 MiB | rigtorp::SPSCQueue | 13.3 | 75.5 |
| 48 KiB | 8 MiB | atomic_queue | 12.7 | 77.8 |
| 48 KiB | 16 MiB | TwoBufQueue | 12.6 | 81.9 |
| 48 KiB | 16 MiB | ReaderWriterQueue | 14.6 | 108.6 |
| 48 KiB | 16 MiB | rigtorp::SPSCQueue | 14.5 | 142.9 |
| 48 KiB | 16 MiB | atomic_queue | 14.0 | 145.5 |
| 96 KiB | 1 MiB | TwoBufQueue | 14.4 | 26.5 |
| 96 KiB | 1 MiB | ReaderWriterQueue | 14.0 | 45.0 |
| 96 KiB | 1 MiB | rigtorp::SPSCQueue | 14.5 | 32.1 |
| 96 KiB | 1 MiB | atomic_queue | 17.9 | 53.5 |
| 96 KiB | 2 MiB | TwoBufQueue | 14.5 | 29.5 |
| 96 KiB | 2 MiB | ReaderWriterQueue | 13.9 | 51.1 |
| 96 KiB | 2 MiB | rigtorp::SPSCQueue | 14.8 | 37.7 |
| 96 KiB | 2 MiB | atomic_queue | 14.3 | 59.5 |
| 96 KiB | 4 MiB | TwoBufQueue | 15.6 | 31.5 |
| 96 KiB | 4 MiB | ReaderWriterQueue | 15.5 | 91.4 |
| 96 KiB | 4 MiB | rigtorp::SPSCQueue | 14.4 | 47.3 |
| 96 KiB | 4 MiB | atomic_queue | 14.8 | 78.7 |
| 96 KiB | 8 MiB | TwoBufQueue | 14.1 | 35.0 |
| 96 KiB | 8 MiB | ReaderWriterQueue | 14.1 | 120.4 |
| 96 KiB | 8 MiB | rigtorp::SPSCQueue | 14.5 | 90.5 |
| 96 KiB | 8 MiB | atomic_queue | 13.9 | 124.5 |
| 96 KiB | 16 MiB | TwoBufQueue | 14.4 | 75.4 |
| 96 KiB | 16 MiB | ReaderWriterQueue | 14.4 | 225.7 |
| 96 KiB | 16 MiB | rigtorp::SPSCQueue | 16.9 | 271.1 |
| 96 KiB | 16 MiB | atomic_queue | 14.2 | 262.0 |

### 测试参数与原始数据



32 B消息，每个字段随序号变化；业务热身512次；每16次业务计时一组；后64次包含4组。回调持续执行256次计算，不人为暂停。按发送量预填充，发送后未处理数据接近256 MiB容量的80%。

TwoBufQueue采用普通write、整块发布、不扫描当前块，源池4×4 KiB，输出池65,536×4 KiB；独立复制及回调线程。ReaderWriterQueue用try_enqueue及普通成员复制，rigtorp用try_push且不额外添加写预取，atomic_queue启用SPSC。

Apple M3，写线程高QoS，未硬绑核。全部消息通过条数、FIFO、完整32 B校验，同工作集下所有实现的业务校验和一致。当前为工作集筛选，分位数还需更多独立重复。

[全部汇总及选择规则](performance_data/burst_business_sizes_20261012/summary.json) · [运行清单、参数及哈希](performance_data/burst_business_sizes_20261012/manifest.json)

- [24 KiB · baseline · 第1次](performance_data/burst_business_sizes_20261012/raw/24k_baseline_run1.json)
- [24 KiB · TwoBufQueue · 第1次](performance_data/burst_business_sizes_20261012/raw/24k_TwoBufQueue_run1.json)
- [24 KiB · ReaderWriterQueue · 第1次](performance_data/burst_business_sizes_20261012/raw/24k_ReaderWriterQueue_run1.json)
- [24 KiB · rigtorp · 第1次](performance_data/burst_business_sizes_20261012/raw/24k_rigtorp_run1.json)
- [24 KiB · atomic_queue · 第1次](performance_data/burst_business_sizes_20261012/raw/24k_atomic_queue_run1.json)
- [48 KiB · baseline · 第1次](performance_data/burst_business_sizes_20261012/raw/48k_baseline_run1.json)
- [48 KiB · TwoBufQueue · 第1次](performance_data/burst_business_sizes_20261012/raw/48k_TwoBufQueue_run1.json)
- [48 KiB · ReaderWriterQueue · 第1次](performance_data/burst_business_sizes_20261012/raw/48k_ReaderWriterQueue_run1.json)
- [48 KiB · rigtorp · 第1次](performance_data/burst_business_sizes_20261012/raw/48k_rigtorp_run1.json)
- [48 KiB · atomic_queue · 第1次](performance_data/burst_business_sizes_20261012/raw/48k_atomic_queue_run1.json)
- [96 KiB · baseline · 第1次](performance_data/burst_business_sizes_20261012/raw/96k_baseline_run1.json)
- [96 KiB · TwoBufQueue · 第1次](performance_data/burst_business_sizes_20261012/raw/96k_TwoBufQueue_run1.json)
- [96 KiB · ReaderWriterQueue · 第1次](performance_data/burst_business_sizes_20261012/raw/96k_ReaderWriterQueue_run1.json)
- [96 KiB · rigtorp · 第1次](performance_data/burst_business_sizes_20261012/raw/96k_rigtorp_run1.json)
- [96 KiB · atomic_queue · 第1次](performance_data/burst_business_sizes_20261012/raw/96k_atomic_queue_run1.json)
- [48 KiB · baseline · 第2次](performance_data/burst_business_sizes_20261012/raw/48k_baseline_run2.json)
- [48 KiB · TwoBufQueue · 第2次](performance_data/burst_business_sizes_20261012/raw/48k_TwoBufQueue_run2.json)
- [48 KiB · ReaderWriterQueue · 第2次](performance_data/burst_business_sizes_20261012/raw/48k_ReaderWriterQueue_run2.json)
- [48 KiB · rigtorp · 第2次](performance_data/burst_business_sizes_20261012/raw/48k_rigtorp_run2.json)
- [48 KiB · atomic_queue · 第2次](performance_data/burst_business_sizes_20261012/raw/48k_atomic_queue_run2.json)
- [96 KiB · baseline · 第2次](performance_data/burst_business_sizes_20261012/raw/96k_baseline_run2.json)
- [96 KiB · TwoBufQueue · 第2次](performance_data/burst_business_sizes_20261012/raw/96k_TwoBufQueue_run2.json)
- [96 KiB · ReaderWriterQueue · 第2次](performance_data/burst_business_sizes_20261012/raw/96k_ReaderWriterQueue_run2.json)
- [96 KiB · rigtorp · 第2次](performance_data/burst_business_sizes_20261012/raw/96k_rigtorp_run2.json)
- [96 KiB · atomic_queue · 第2次](performance_data/burst_business_sizes_20261012/raw/96k_atomic_queue_run2.json)
- [24 KiB · baseline · 第2次](performance_data/burst_business_sizes_20261012/raw/24k_baseline_run2.json)
- [24 KiB · TwoBufQueue · 第2次](performance_data/burst_business_sizes_20261012/raw/24k_TwoBufQueue_run2.json)
- [24 KiB · ReaderWriterQueue · 第2次](performance_data/burst_business_sizes_20261012/raw/24k_ReaderWriterQueue_run2.json)
- [24 KiB · rigtorp · 第2次](performance_data/burst_business_sizes_20261012/raw/24k_rigtorp_run2.json)
- [24 KiB · atomic_queue · 第2次](performance_data/burst_business_sizes_20261012/raw/24k_atomic_queue_run2.json)
- [96 KiB · baseline · 第3次](performance_data/burst_business_sizes_20261012/raw/96k_baseline_run3.json)
- [96 KiB · TwoBufQueue · 第3次](performance_data/burst_business_sizes_20261012/raw/96k_TwoBufQueue_run3.json)
- [96 KiB · ReaderWriterQueue · 第3次](performance_data/burst_business_sizes_20261012/raw/96k_ReaderWriterQueue_run3.json)
- [96 KiB · rigtorp · 第3次](performance_data/burst_business_sizes_20261012/raw/96k_rigtorp_run3.json)
- [96 KiB · atomic_queue · 第3次](performance_data/burst_business_sizes_20261012/raw/96k_atomic_queue_run3.json)
- [24 KiB · baseline · 第3次](performance_data/burst_business_sizes_20261012/raw/24k_baseline_run3.json)
- [24 KiB · TwoBufQueue · 第3次](performance_data/burst_business_sizes_20261012/raw/24k_TwoBufQueue_run3.json)
- [24 KiB · ReaderWriterQueue · 第3次](performance_data/burst_business_sizes_20261012/raw/24k_ReaderWriterQueue_run3.json)
- [24 KiB · rigtorp · 第3次](performance_data/burst_business_sizes_20261012/raw/24k_rigtorp_run3.json)
- [24 KiB · atomic_queue · 第3次](performance_data/burst_business_sizes_20261012/raw/24k_atomic_queue_run3.json)
- [48 KiB · baseline · 第3次](performance_data/burst_business_sizes_20261012/raw/48k_baseline_run3.json)
- [48 KiB · TwoBufQueue · 第3次](performance_data/burst_business_sizes_20261012/raw/48k_TwoBufQueue_run3.json)
- [48 KiB · ReaderWriterQueue · 第3次](performance_data/burst_business_sizes_20261012/raw/48k_ReaderWriterQueue_run3.json)
- [48 KiB · rigtorp · 第3次](performance_data/burst_business_sizes_20261012/raw/48k_rigtorp_run3.json)
- [48 KiB · atomic_queue · 第3次](performance_data/burst_business_sizes_20261012/raw/48k_atomic_queue_run3.json)
