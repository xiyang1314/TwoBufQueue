# Performance evidence / 性能证据

[README](../README.md) · [完整可调测试文档](../benchmarks/TUNABLE_SPSC.zh-CN.md)

## Current comparison entry points / 当前对比入口

| Experiment | Report | What is timed |
|---|---|---|
| 256 MiB, WriteSession, empty callback | [Session](performance_256m_zero_session_plateau.html) | Producer submission, including session acquisition and switching |
| 256 MiB, ordinary write, maximum publish count | [Block-handoff publication](performance_256m_zero_publish_max_plateau.html) | Producer submission, including block acquisition and switching |
| 256 MiB, ordinary write, publication every record | [Per-record publication](performance_256m_zero_publish1_plateau.html) | Producer submission, including publication |
| Continuous message batches, each starting empty | [Business performance after a batch](performance_burst_from_empty.html) | Business execution immediately after sending; ratio to normal business cost |
| Earlier experiment: prepared message backlog | [Prepared-backlog business execution](performance_burst_cache.html) | Same business workload with approximately 198–205 MiB pending |

The first three reports use three independent runs, callback loop count 0 and one common block configuration per report across 8/16/32/64 B. Configuration selection uses the stated 35% throughput, 30% p50, 20% p99 and 15% p999 weights. The current business experiment starts every batch empty and uses regular upstream SPSC APIs with fixed callback work. Business p50 reflects 16-call group means; the write reports use 1,000-write group means. The two business reports have different initial backlog states.

当前三个写入对比报告分别展示 Session、仅交接发布、每条发布；每份报告四种长度使用同一组参数，并公开所有初始化参数。业务对比当前入口是“连续发送一批消息，对业务性能产生什么影响”，每批从零积压开始。此前预填充大积压的报告保留为独立历史实验。正式测试记录以各报告保存的配置和全部次数为准。

## Business performance after continuous sending / 连续发送后的业务性能

Flow: normal business execution → continuous message batch → immediate business execution. Each batch starts with zero pending messages, consumers stay active, and draining happens outside timing. The setup uses 32 B messages, a 48 KiB business working set, 64 callback iterations, a 16 KiB TwoBufQueue source pool and 256 MiB output/native capacity. Every 2/4/8/16 MiB size has three independent runs, 2,500 windows and 10,000 business timing groups per run. [Full invariant and sample verification](performance_data/burst_from_empty_20261013/validation.json).

当前表格显示发送后的业务 p50：每组计时 16 次业务并除以 16，各轮独立计算 p50，再对三轮取平均。正常业务参考值来自同批无队列对照，本机显示为约 **13 ns/次**；“比值”统一采用发送后业务耗时 ÷ 13 ns。图表、README 和报告均使用[这次完整结果](performance_data/burst_from_empty_20261013/formal/report_results.json)，没有重新挑选测试次数。

第一层与第二层容量分别计算，消费者与复制线程持续运行。每批发送后会自然产生未处理消息，但不准备约 200 MiB 初始积压。旧的 80% 积压准备规则仅属于[历史预填充实验](performance_burst_cache.html)；它的[历史图表](assets/burst_business_cost_prefilled.svg)及原始结果保留。

From the project root, replay the current fixed workload or regenerate its figure:

```sh
python3 benchmarks/run_burst_empty.py --profile docs/performance_data/burst_from_empty_20261013/selected_config.json
python3 -m pip install reportlab==4.4.9
python3 tools/plot_burst_release.py
```

The plot command reads saved results and updates `docs/assets/burst_business_cost.svg` and its numeric JSON; it does not run performance tests. The original measurements and their hashes remain unchanged.

[Run the batch/business experiment](../benchmarks/BURST_CACHE.zh-CN.md) · [Search writer configurations](../benchmarks/TUNABLE_SPSC.zh-CN.md) · [Browse all published reports](index.html).

## Compressed evidence / 压缩证据

The release exporter retains original measurements byte for byte inside deterministic gzip files when they exceed 1 MiB. HTML and Markdown evidence links in the release copy point to the compressed files. `compression-manifest.json` records both original and compressed SHA-256 hashes. To restore the recorded file paths without overwriting different existing files:

```sh
python tools/unpack_performance_data.py
```

The original development directory remains unchanged by export. Source/executable archives and original absolute command paths are evidence of the measurement host; use the portable benchmark runners to measure another host. Publication packaging and compression do not create new benchmark results.

## Historical matched measurements / 历史同参数测量

The historical producer-write chart uses the saved 2026-10-11 post-fix matched-parameter experiment, not newly selected best results. Both ordinary write and single-block Session use the same block sizes, pool limits, publication settings and read configuration for each payload length. Throughput and latency use the same configuration; the results are not claimed to be global optima.

历史写入图来自提交 `f698f25` 后重新测量的同参数实验，没有重新选择更好的次数。每个长度的普通 write 与 Session 块大小、池容量、发布与读端配置一致。主表保留所有正式结果，不挑选最好的一次。

| Condition | Ordinary write | WriteSession |
|---|---|---|
| 8 MiB per layer, callback iterations 0 | [Report](performance_8m_zero.html) | [Report](performance_8m_zero_session.html) |
| 256 MiB per layer, calibrated callback work | [Report](performance_256m_callback.html) | [Report](performance_256m_callback_session.html) |

## Latest ordinary-write scan-current run / 普通 write 开启当前块扫描

普通 write 已单独重新测量：`is_scan_current=true`，初始/最大扫描间隔和最小等待均为 **1 ms**，`publish_count=10`。每项 1,000 万条，吞吐和分组延时各 5 次；320 次正式测量、32 次完整负载/FIFO 校验全部通过。256 MiB 的回调循环仍为 1/1/2/4 次，8 MiB 为 0，以隔离读端/发布参数变化，未重新校准到 2～3 倍。原生 SPSC 也在本轮重新测量。

上方历史四报告表中的普通 write 链接指向本轮；Session 报告及其数据保留上一轮，两种模式当前并非同参数、同轮次对照。历史写入成本图仍来自下方记录的关闭 current 扫描、UINT32_MAX 发布同参数实验，未混入新数据。旧报告备份保留在开发目录；公开指标以各报告保存的配置为准。

[本轮原始数据](performance_data/normal_current_scan10_20261011/raw.jsonl.gz) · [本轮环境与源码哈希](performance_data/normal_current_scan10_20261011/environment.json) · [源码快照](performance_data/normal_current_scan10_20261011/source.tar.gz) · [测试后核对](performance_data/normal_current_scan10_20261011/post-run-audit.json)。以下口径及历史实验记录以各自保存的配置为准；本轮仅上述扫描/发布配置不同。

## Previous matched experiment scope / 前一轮同参数实验口径

- Apple M3, 8 GiB RAM, macOS 14.4.1 arm64, Clang 18.1.8, optimized unsanitized Release, AC power. Producer high QoS; no hard affinity. Other platforms must be measured independently.
- 8/16/32/64-byte payloads; 10 million successful writes per formal run, five throughput runs and five latency runs per entry. Input is prepared before timing and every message's bytes vary. Full-payload/FIFO validation runs are separate from measured runs.
- Latency is the distribution of average write cost over groups of 1,000 writes. The table reports the arithmetic mean of five runs' p50/p99/p999 values. It is not individual-call latency or time to callback. The historical producer-write chart shows the mean p50; all five values are retained in the chart data.
- Throughput uses accumulated producer send time across rounds. Each round drains before the next; draining time is excluded, but copying/callback threads may contend during sending. Failed attempts, waiting and block switching stay in the send timing.
- TwoBufQueue includes layer-2 copying and an independent callback thread. Each layer has its own payload budget, so 8 MiB per layer means 16 MiB total. Native SPSC comparisons have one 8 MiB payload budget, including reserved slots/padding. Queue metadata, prepared input and other process memory are reported separately.
- Current-block scanning is disabled; publish count is `UINT32_MAX`. There is no per-message flush. Tail handoff happens outside round send timing; Session acquisition and switching stay inside write timing.
- For 256 MiB, callback loop work is calibrated against ordinary TwoBufQueue write to approximately 2–3 times producer wall time. The same loop count is then fixed for that length across all transports and Session. Their actual callback/write ratio can differ; the ratio is not a per-call callback latency ratio.

The p50/p99/p999 samples, CPU/RSS, backpressure, round counts and actual parameters are in each report. The finite sample count and timer granularity constrain tail interpretation. These conditions emphasize producer submission cost, not equal-thread-count or equal-total-memory competition.

## Copy-constructor sensitivity

All implementations copy the complete variable payload. The ordinary copy constructor is `bytes(other.bytes)`. ReaderWriterQueue 16/32 B uses a tagged `CopyAfterZero` constructor through the original `try_emplace`: default-zero the destination, then `memcpy` the full payload once; both operations are timed. Its 8/64 B paths retain member copying. rigtorp optionally requests a destination write-prefetch hint through its original `try_emplace` API, followed by one complete member copy, with no extra zeroing. These per-transport choices are fixed before timing and disclosed in each row and the complete parameters. See the [rigtorp control](RIGTORP_PERFORMANCE.md) and [native copy control](performance_data/native_copy_control_20261011/summary.json). Upstream headers remain unchanged and are checked against pinned hashes. The earlier [independent A/B](performance_data/copy_sensitivity_20261011/summary.json) changes only the external message copy constructor; validation passes but results change markedly. The precise processor-level explanation has not been established.

主表公开保留各调用端优化，不采用统一复制策略：ReaderWriterQueue 16/32 B 含先清零再复制，rigtorp 含目标写预取。排名同时受外部 Payload 与编译器代码生成影响，不能全归因于队列算法；顶部历史 A/B 也不混入本轮正式数据。

### Earlier 16-byte follow-up / 写预取修正前的 16 B 复测

The follow-up uses the same two measured binaries and arguments: 8 MiB, callback iterations 0, 10 million messages per run, alternating constructor variants, five throughput runs and five latency runs per variant/queue. Four separate full-payload validation runs passed. Upstream queue implementations are unchanged.

| Queue | External Payload copy constructor | Throughput M messages/s | Group-average p50 ns |
|---|---|---:|---:|
| ReaderWriterQueue | Member initialization: `bytes(other.bytes)` | 97.12 | 9.308 |
| ReaderWriterQueue | Constructor-body `memcpy` | 480.01 | 1.292 |
| rigtorp/SPSCQueue | Member initialization: `bytes(other.bytes)` | 42.84 | 23.075 |
| rigtorp/SPSCQueue | Constructor-body `memcpy` | 368.86 | 2.608 |

These are arithmetic means of five independent runs per metric. The p50 is a quantile of 1,000-write group averages, not single-message or end-to-end latency. Complete commands, executable hashes and all 44 runs are retained in the [summary](performance_data/copy_sensitivity_20261011/16b-recheck/summary.json), [metadata](performance_data/copy_sensitivity_20261011/16b-recheck/metadata.json) and [raw data](performance_data/copy_sensitivity_20261011/16b-recheck/raw.jsonl). The original measured and variant sources are in the archives linked below.

此前复测重现了 16 B 的复制构造敏感性。rigtorp 的函数体 `memcpy` 吞吐恢复到旧报告约 363 M条/s 的水平；ReaderWriterQueue 的 p50 恢复到约 1.292 ns，但吞吐约 480 M条/s，仍低于旧报告约 742 M条/s。这是此前修正前的实验；最新主表为 ReaderWriterQueue 16/32 B 显式保留先清零再复制路径。复制构造触发的精确处理器/编译器机制仍未由硬件计数器定位。不能把 8 B 的局部改善概括为所有长度都提速，也不能据此声称 TwoBufQueue 有如此大的队列算法优势。最新主报告使用修正后的程序重新测量，未用历史成绩替换。rigtorp 的回退已通过调用端可选目标写预取消除；同一程序开关对照及结论边界见 [修正说明](RIGTORP_PERFORMANCE.md)。

## Reproduce and inspect

The [runner](../benchmarks/run_release_benchmarks.py) records original source snapshots, executable/source/dependency hashes, compiler flags, commands, calibration and all trials. Resume requires identical inputs. Build with `BRQ_BUILD_SPSC_BENCHMARKS=ON`; dependencies are benchmark-only and explicitly downloaded by `bootstrap_spsc.py`, never by default configure. Follow [the complete commands](../benchmarks/TUNABLE_SPSC.zh-CN.md).

[Formal raw data](performance_data/release_20261011_inline_fixed_110024/raw.jsonl.gz) · [environment](performance_data/release_20261011_inline_fixed_110024/environment.json) · [original measured source](performance_data/release_20261011_inline_fixed_110024/source.tar.gz) · [copy-control raw data](performance_data/copy_sensitivity_20261011/raw.jsonl)

The previous four-report matched experiment was measured from commit `f698f25`, which removes forced inlining from six benchmark lambdas while leaving the queue library unchanged. The [8 B regression evidence](performance_data/twobufqueue_8b_regression_20261011/README.txt) records the controls and limits. This run retains 400 independent formal measurements, 40 separate complete-payload/FIFO validations and 45 callback calibration runs; all completed successfully. The 256 MiB callback loop counts for 8/16/32/64 B are 1/1/2/4, shared across all transports and Session. Calibration ratios are 3.09/2.17/1.95/2.38; 8 B and 32 B slightly miss the 2–3 target and remain disclosed. Native controls are shared between ordinary/Session reports of the same cache size, so report counts cannot be added together. Exact measured-source hashes and the source archive are retained; scratch/report files explain the dirty worktree flag. Saved absolute command paths identify the measurement host, not portable instructions.

Regenerate the historical producer-write figure with Python 3.9+ and ReportLab 4.4.9:

```sh
python -m pip install reportlab==4.4.9
python benchmarks/plot_release.py --data docs/performance_data/release_20261011_inline_fixed_110024
```

The [plot script](../benchmarks/plot_release.py) reads the original two results files, exports [SVG](assets/write_cost_8m.svg), and saves its [numeric chart data](assets/write_cost_8m.json). It does not run benchmarks or change measurement data.
