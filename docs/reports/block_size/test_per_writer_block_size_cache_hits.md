# 两种发布策略下，各 block size 的 writer L1D 缓存诊断

**配置 A：** 扫描 current，间隔 1 ms，每条发布，`publish_count=1`。 · **配置 B：** 不扫描 current，`publish_count=UINT32_MAX`，仅交接块时发布。

32 B 消息，每次一千万条，回调循环 0；writer 块 1 KiB 至 1 GiB，reader 块始终为 writer 的 10 倍。writer 池上限 2 GiB，reader 基准上限 2 GiB，按既有规则至少容纳一个完整 reader 块。按需分配、约 6.4 MiB 分轮发送和排空逻辑与前两份报告一致。高 QoS，未硬绑核。

采集范围为 **当前写线程每轮正式发送**：包括普通 write、自然换块、重试、原有池深度统计和快照/时钟之间的少量边界开销。数据准备、预热、轮间排空、轮末交接、复制线程和回调线程不计入。生产库和写函数未修改，新增计数仅位于轮次边界。

**这些百分比是“1 − 未命中/µop”的数学归一化比例，不能当作精确 L1 命中率。** 本机没有同口径的 L1 总访问事件。公式为 `100 × (1 − L1D_CACHE_MISS_LD / LD_UNIT_UOP)`，写入对应 ST 事件。分母是经过读写单元的微操作，不是缓存查询总数；一般事件可以包括推测执行和重试。报告同时保存未命中次数，供直接比较相同业务量。

校验发现：成对向量指令 `ldp/stp` 的退休指令可计为一次，而缓存未命中可计为两次。因此不能用“退休未命中 ÷ 整数与 SIMD 退休访存指令”计算命中率。这里保留两组退休事件作为辅助指标，但不使用它们作为命中率分母。

统计 writer 线程所有读写单元活动，包含输入数据、目标 block 和 writer 状态，硬件不能按 block 地址过滤。读取/写入因 PMU 槽位约束分别独立跑五次，无事件复用；百分比为五次算术平均，加权比例及范围保存在 CSV/JSON。

**L2 数据缓存命中率无法测量：** 本机 as1 事件表没有 L2 数据缓存的访问、命中或未命中事件；L2 TLB 未命中不是 L2 缓存未命中。

成对写入的 4 KiB hot 校准中，未命中/µop 比例为 5.14%–9.37%，没有满足事先设置的低于 1% 检查阈值。这一现象尚未解释，写入方向不宣称得到可信命中率。下表仅展示原始事件的数学归一化比例；它不能证明物理缓存查询的命中率。

## L1D 微操作归一化比例对照（非精确命中率）

| writer 块 | reader 块 | A 读非 miss/µop | A 写非 miss/µop | B 读非 miss/µop | B 写非 miss/µop | 读差异 / 百分点 | 写差异 / 百分点 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 KiB | 10 KiB | 91.678% | 89.995% | 87.458% | 89.343% | -4.220 | -0.652 |
| 2 KiB | 20 KiB | 92.558% | 93.035% | 87.490% | 92.554% | -5.068 | -0.481 |
| 4 KiB | 40 KiB | 93.040% | 82.580% | 88.671% | 79.554% | -4.369 | -3.026 |
| 8 KiB | 80 KiB | 91.470% | 73.157% | 88.594% | 66.832% | -2.875 | -6.325 |
| 16 KiB | 160 KiB | 91.542% | 61.305% | 88.480% | 58.753% | -3.062 | -2.552 |
| 32 KiB | 320 KiB | 92.133% | 51.974% | 88.010% | 56.454% | -4.122 | +4.480 |
| 64 KiB | 640 KiB | 91.822% | 43.238% | 87.574% | 53.150% | -4.248 | +9.911 |
| 128 KiB | 1.25 MiB | 91.033% | 43.079% | 87.322% | 51.082% | -3.711 | +8.003 |
| 256 KiB | 2.5 MiB | 90.970% | 40.238% | 87.302% | 50.838% | -3.668 | +10.600 |
| 512 KiB | 5 MiB | 90.911% | 36.002% | 87.379% | 50.505% | -3.532 | +14.503 |
| 1 MiB | 10 MiB | 91.347% | 34.714% | 87.424% | 50.401% | -3.924 | +15.686 |
| 2 MiB | 20 MiB | 91.451% | 42.679% | 87.577% | 50.441% | -3.874 | +7.762 |
| 4 MiB | 40 MiB | 91.663% | 38.389% | 87.842% | 50.289% | -3.821 | +11.900 |
| 8 MiB | 80 MiB | 91.660% | 34.724% | 87.868% | 50.176% | -3.792 | +15.452 |
| 16 MiB | 160 MiB | 91.834% | 36.754% | 87.905% | 50.241% | -3.928 | +13.488 |
| 32 MiB | 320 MiB | 91.457% | 38.744% | 87.887% | 50.300% | -3.570 | +11.555 |
| 64 MiB | 640 MiB | 91.886% | 39.553% | 87.857% | 50.447% | -4.029 | +10.894 |
| 128 MiB | 1.25 GiB | 92.227% | 45.813% | 87.858% | 50.281% | -4.369 | +4.469 |
| 256 MiB | 2.5 GiB | 92.693% | 57.692% | 87.910% | 50.243% | -4.783 | -7.449 |
| 512 MiB | 5 GiB | 93.182% | 59.557% | 87.910% | 50.631% | -5.272 | -8.926 |
| 1 GiB | 10 GiB | 92.899% | 57.156% | 87.877% | 50.410% | -5.023 | -6.745 |

## 同样消息量下的未命中次数

发布每条消息会增加状态访存，因此百分比高也可能只是分母更大。每千条消息的 miss 次数可避免这类分母变化；微操作次数包含推测执行和重试，不代表每条消息的静态指令数。

| writer 块 | A 读 miss / 千条 | A 读 µop / 条 | A 写 miss / 千条 | A 写 µop / 条 | B 读 miss / 千条 | B 读 µop / 条 | B 写 miss / 千条 | B 写 µop / 条 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 KiB | 1,476.79 | 17.745 | 879.50 | 8.722 | 1,654.19 | 13.189 | 1,059.57 | 9.956 |
| 2 KiB | 910.36 | 12.231 | 400.07 | 5.744 | 1,082.04 | 8.648 | 510.04 | 6.849 |
| 4 KiB | 733.47 | 10.537 | 842.04 | 4.834 | 733.51 | 6.474 | 1,117.93 | 5.469 |
| 8 KiB | 1,182.07 | 13.857 | 1,734.18 | 6.460 | 633.43 | 5.554 | 1,604.20 | 4.837 |
| 16 KiB | 937.54 | 11.087 | 1,910.18 | 4.937 | 571.02 | 4.956 | 1,811.79 | 4.393 |
| 32 KiB | 733.48 | 9.323 | 1,903.41 | 3.962 | 636.29 | 5.307 | 1,950.64 | 4.480 |
| 64 KiB | 693.57 | 8.481 | 1,992.80 | 3.511 | 634.94 | 5.110 | 2,039.70 | 4.354 |
| 128 KiB | 718.30 | 8.011 | 1,874.89 | 3.304 | 611.30 | 4.822 | 2,040.57 | 4.171 |
| 256 KiB | 706.31 | 7.819 | 1,898.04 | 3.180 | 602.85 | 4.747 | 2,029.70 | 4.129 |
| 512 KiB | 706.98 | 7.767 | 1,985.71 | 3.103 | 587.48 | 4.654 | 2,017.26 | 4.076 |
| 1 MiB | 661.58 | 7.646 | 2,008.41 | 3.076 | 580.16 | 4.613 | 2,011.89 | 4.056 |
| 2 MiB | 650.02 | 7.603 | 1,783.40 | 3.120 | 570.17 | 4.590 | 2,009.04 | 4.054 |
| 4 MiB | 632.03 | 7.581 | 1,909.14 | 3.103 | 553.49 | 4.553 | 2,002.78 | 4.029 |
| 8 MiB | 634.66 | 7.610 | 2,000.58 | 3.065 | 552.45 | 4.554 | 2,003.62 | 4.021 |
| 16 MiB | 632.36 | 7.744 | 1,975.17 | 3.123 | 550.15 | 4.549 | 2,003.45 | 4.026 |
| 32 MiB | 685.23 | 7.986 | 1,967.11 | 3.212 | 551.00 | 4.549 | 1,999.99 | 4.024 |
| 64 MiB | 653.27 | 8.052 | 1,986.32 | 3.308 | 553.38 | 4.557 | 1,993.69 | 4.023 |
| 128 MiB | 704.62 | 9.090 | 1,949.51 | 3.641 | 552.58 | 4.551 | 2,005.28 | 4.033 |
| 256 MiB | 706.79 | 9.705 | 2,253.98 | 5.366 | 552.46 | 4.569 | 2,004.71 | 4.029 |
| 512 MiB | 737.39 | 10.881 | 2,374.34 | 5.875 | 552.43 | 4.569 | 1,998.98 | 4.049 |
| 1 GiB | 752.59 | 10.607 | 2,371.59 | 5.576 | 557.71 | 4.600 | 2,009.29 | 4.052 |

## 退休未命中、L1 脏行回写与采集时吞吐

每组依次列出退休 miss/千条、脏缓存行回写/千条、M条/s。load 与 store 是独立执行；两组 writeback 的差异表示运行波动。脏行回写计数不是延时，也不是所有跨核一致性流量。PMU 吞吐仅供诊断，原性能报告保留不变。

| writer 块 | A 读 miss | A 读组回写 | A 读组吞吐 | A 写 miss | A 写组回写 | A 写组吞吐 | B 读 miss | B 读组回写 | B 读组吞吐 | B 写 miss | B 写组回写 | B 写组吞吐 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 KiB | 433.27 | 879.73 | 100.957 | 624.38 | 878.26 | 96.365 | 384.64 | 893.11 | 111.389 | 774.82 | 888.18 | 112.681 |
| 2 KiB | 253.71 | 700.15 | 164.556 | 242.71 | 700.00 | 166.085 | 242.26 | 700.19 | 196.119 | 365.80 | 699.61 | 199.139 |
| 4 KiB | 173.26 | 502.41 | 80.716 | 762.34 | 499.25 | 81.391 | 123.81 | 414.53 | 279.917 | 1,010.57 | 420.06 | 267.540 |
| 8 KiB | 539.01 | 443.00 | 43.908 | 1,665.52 | 444.18 | 42.822 | 77.27 | 212.97 | 344.054 | 1,539.44 | 212.37 | 340.217 |
| 16 KiB | 376.53 | 271.50 | 36.770 | 1,876.24 | 262.07 | 37.392 | 52.53 | 110.43 | 406.898 | 1,781.21 | 112.34 | 397.189 |
| 32 KiB | 241.15 | 133.96 | 36.242 | 1,888.62 | 155.82 | 33.691 | 112.43 | 80.72 | 398.586 | 1,924.37 | 79.26 | 403.887 |
| 64 KiB | 219.29 | 76.21 | 30.104 | 1,982.47 | 82.01 | 27.483 | 117.11 | 51.80 | 405.348 | 2,023.47 | 52.29 | 404.358 |
| 128 KiB | 287.84 | 57.78 | 23.053 | 1,861.83 | 100.22 | 24.086 | 103.07 | 37.83 | 439.618 | 2,028.06 | 37.29 | 434.820 |
| 256 KiB | 290.95 | 50.39 | 18.667 | 1,889.22 | 68.67 | 20.253 | 99.74 | 28.50 | 442.166 | 2,022.72 | 26.78 | 448.288 |
| 512 KiB | 275.72 | 58.70 | 18.859 | 1,979.94 | 31.52 | 16.921 | 88.87 | 20.45 | 446.926 | 2,012.30 | 18.25 | 457.916 |
| 1 MiB | 267.41 | 24.41 | 15.824 | 2,003.23 | 20.46 | 16.435 | 86.06 | 13.60 | 472.634 | 2,008.31 | 13.35 | 473.253 |
| 2 MiB | 254.32 | 21.06 | 18.683 | 1,776.47 | 81.67 | 21.285 | 78.46 | 11.41 | 489.811 | 2,005.87 | 11.98 | 481.695 |
| 4 MiB | 233.14 | 15.46 | 20.755 | 1,902.22 | 47.34 | 22.721 | 65.69 | 9.80 | 507.611 | 2,000.37 | 10.44 | 509.485 |
| 8 MiB | 234.11 | 18.37 | 20.314 | 1,996.90 | 15.56 | 20.600 | 64.43 | 9.76 | 520.300 | 2,001.40 | 8.99 | 519.692 |
| 16 MiB | 227.82 | 19.53 | 20.812 | 1,969.65 | 25.49 | 21.632 | 62.66 | 9.44 | 534.221 | 2,001.08 | 9.41 | 529.439 |
| 32 MiB | 220.64 | 69.68 | 24.131 | 1,959.72 | 32.74 | 22.499 | 62.90 | 9.72 | 521.688 | 1,997.69 | 10.31 | 513.793 |
| 64 MiB | 232.53 | 40.00 | 23.154 | 1,978.50 | 33.24 | 22.016 | 63.51 | 10.50 | 515.278 | 1,991.41 | 11.98 | 517.115 |
| 128 MiB | 244.99 | 77.93 | 23.694 | 1,935.98 | 61.05 | 24.022 | 64.10 | 9.44 | 521.977 | 2,002.81 | 9.68 | 517.416 |
| 256 MiB | 243.88 | 84.63 | 24.699 | 2,219.35 | 115.78 | 24.487 | 63.82 | 10.15 | 488.952 | 2,002.24 | 9.50 | 515.626 |
| 512 MiB | 257.46 | 103.65 | 24.681 | 2,333.51 | 127.04 | 24.545 | 64.41 | 9.63 | 517.364 | 1,996.33 | 12.79 | 502.864 |
| 1 GiB | 257.07 | 113.51 | 25.313 | 2,337.72 | 103.72 | 24.252 | 65.42 | 13.99 | 495.324 | 2,006.67 | 10.08 | 502.896 |

## 独立汇编计数单位校验

scalar：8 B 标量；single：16 B 向量；pair：32 B 向量对。hot 工作集 4 KiB，stream 工作集 4 MiB。每个用例三次，每次两百万次汇编访存操作。hot 未命中/µop 检查阈值为 1%，成对写入未达到该阈值；成对操作与退休指令计数的区别可由下表直接核对。校验能发现明显单位错误，不能证明微操作计数就是全部缓存查询数。

| 方向 | 形态 | 工作集 | 退休访存指令 / 操作 | 单元 µop / 操作 | 退休 miss / 操作 | 非 miss/µop | 检查 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| load | scalar | hot | 2.00239 | 2.00552 | 0.00025 | 99.981% | 数值检查通过 |
| load | scalar | stream | 2.03615 | 2.55835 | 0.32425 | 80.363% | 数值检查通过 |
| load | single | hot | 2.00139 | 2.00374 | 0.00010 | 99.994% | 数值检查通过 |
| load | single | stream | 2.00139 | 2.47825 | 0.28457 | 81.857% | 数值检查通过 |
| load | pair | hot | 2.00126 | 3.11958 | 0.00009 | 99.996% | 数值检查通过 |
| load | pair | stream | 2.00177 | 3.35831 | 0.07580 | 95.264% | 数值检查通过 |
| store | scalar | hot | 1.00059 | 1.00079 | 0.00002 | 99.998% | 数值检查通过 |
| store | scalar | stream | 1.00081 | 1.44740 | 0.16339 | 59.879% | 数值检查通过 |
| store | single | hot | 1.00080 | 1.00116 | 0.00009 | 99.990% | 数值检查通过 |
| store | single | stream | 1.00059 | 1.44362 | 0.21670 | 56.427% | 数值检查通过 |
| store | pair | hot | 1.01044 | 2.01907 | 0.13464 | 93.298% | hot 检查未通过 |
| store | pair | stream | 1.00059 | 2.02048 | 1.98958 | 1.529% | 数值检查通过 |

完整队列另做 42 次全字节/FIFO 校验；正式计数共 420 次，校准共 36 次。

[CSV 汇总](../../performance_data/writer_block_size_cache_hits_20261011/summary.csv) · [JSON 汇总及五次计数](../../performance_data/writer_block_size_cache_hits_20261011/summary.json.gz) · [原始运行记录](../../performance_data/writer_block_size_cache_hits_20261011/raw.jsonl.gz) · [原始单位校验](../../performance_data/writer_block_size_cache_hits_20261011/calibration.json)

### 环境、事件定义、测量边界与文件指纹



```
{
  "timestamp": "2026-10-11T16:10:50.235547+08:00",
  "platform": "macOS-14.4.1-arm64-arm-64bit",
  "cpu": {
    "command": [
      "sysctl",
      "-n",
      "machdep.cpu.brand_string"
    ],
    "exit_code": 0,
    "stdout": "Apple M3\n",
    "stderr": ""
  },
  "cases": {
    "scan_publish1": [
      true,
      1
    ],
    "completed_publishmax": [
      false,
      4294967295
    ]
  },
  "writer_block_sizes": [
    1024,
    2048,
    4096,
    8192,
    16384,
    32768,
    65536,
    131072,
    262144,
    524288,
    1048576,
    2097152,
    4194304,
    8388608,
    16777216,
    33554432,
    67108864,
    134217728,
    268435456,
    536870912,
    1073741824
  ],
  "messages": 10000000,
  "callback_iterations": 0,
  "repeats": 5,
  "reader_block_multiplier": 10,
  "writer_cache_limit_bytes": 2147483648,
  "reader_base_limit_bytes": 2147483648,
  "round_storage_bytes": 6710886,
  "qos": "user_interactive",
  "hard_cpu_affinity": false,
  "counter_scope": "calling writer thread, snapshots enclosing each formal producer round",
  "excluded": "preparation, warmup, inter-round drain, finish_block, copy thread and callback thread",
  "included": "write API, natural block switch, pool-depth instrumentation, retry, tiny snapshot/timer boundary overhead",
  "separate_groups": "load/store gathered in separate executions due to PMU slot masks; no multiplexing",
  "hit_estimate_formula": "100 * (1 - L1D_CACHE_MISS_{LD,ST} / {LD,ST}_UNIT_UOP)",
  "metric_limitation": "This is a uop-normalized estimate, not an exact cache lookup hit rate: no matching L1 total-access event exists in this machine event table. Unit uops and general misses can include speculative work and retries.",
  "rejected_formula": "MISS_NONSPEC / (INST_INT + INST_SIMD): pair instructions count once, but misses can count both accesses, as the retained calibration demonstrates.",
  "address_scope": "all load/store-unit activity on writer thread, includes input payload and writer state; no address filtering",
  "L2_cache_hit_rate": null,
  "L2_reason": "as1 event database has no L2 data-cache access/hit/miss events; L2 TLB events are unrelated",
  "power_before": {
    "command": [
      "pmset",
      "-g",
      "batt"
    ],
    "exit_code": 0,
    "stdout": "Now drawing from 'AC Power'\n -InternalBattery-0 (id=6226019)\t100%; charged; 0:00 remaining present: true\n",
    "stderr": ""
  },
  "memory_before": {
    "vm_stat": {
      "command": [
        "vm_stat"
      ],
      "exit_code": 0,
      "stdout": "Mach Virtual Memory Statistics: (page size of 16384 bytes)\nPages free:                                2727.\nPages active:                             70055.\nPages inactive:                           64257.\nPages speculative:                         5220.\nPages throttled:                              0.\nPages wired down:                        128123.\nPages purgeable:                              2.\n\"Translation faults\":                4534432466.\nPages copy-on-write:                   50804245.\nPages zero filled:                   1716001886.\nPages reactivated:                   1602891411.\nPages purged:                         617476733.\nFile-backed pages:                        52854.\nAnonymous pages:                          86678.\nPages stored in compressor:             1330417.\nPages occupied by compressor:            201521.\nDecompressions:                      2181791239.\nCompressions:                        2460504047.\nPageins:                              180799447.\nPageouts:                               1558493.\nSwapins:                               59764928.\nSwapouts:                              62141712.\n",
      "stderr": ""
    },
    "swap": {
      "command": [
        "sysctl",
        "vm.swapusage"
      ],
      "exit_code": 0,
      "stdout": "vm.swapusage: total = 10240.00M  used = 9711.69M  free = 528.31M  (encrypted)\n",
      "stderr": ""
    }
  },
  "source_and_binary_sha256": {
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/writer_cache_hits_20261011/test_per_writer_block_size_pmu": "5cae4ca1c1142925f43ac91604c3f6926e406688e119a96b3a2ed8aea8501d46",
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/writer_cache_hits_20261011/calibration": "efb8fba58e91f38aa7407a57b74cc0b31164d0dd199e27f364f1e4429ed4c5e5",
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/writer_cache_hits_20261011/pmu_control": "cbfa91eba6022c0cf6a6efcf0f07e985a03f66255dfa636b04623b0ff2134bf2",
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/writer_cache_hits_20261011/pmu_bridge.c": "5a38d39af3bf6b1aee9ad306aefb9c2c7eaf72fd577d4a5a89a1a1876ca6c0d2",
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/writer_cache_hits_20261011/producer_pmu.hpp": "74a210dce499275e240d6f55589a0b7ec2060a3e730c37705f9953c2c88105c7",
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/writer_cache_hits_20261011/calibration.cpp": "164050705440df75e64ef27f0af1b510683fce76c710e1ca7f588f4ae1c5c554",
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/writer_cache_hits_20261011/pmu_control.c": "7040779033b14b473d1fd7b173e52c4b3db6b236ebcf916fa03882d80a131966",
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/writer_cache_hits_20261011/compile-command.json": "7d90626e10f39d6fc88ac95dc9de448be3aaefaa16a4ffaeb3408b061339ea9b",
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/writer_cache_hits_20261011/link-command.json": "6f51983bcb11df5fa0375468ed0eeb3c22e09ade6ebaeee8229cbd437029ae5d",
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/writer_cache_hits_20261011/overlay/test_per_writer_block_size_pmu.cpp": "42be25672e8ee70a3cc50037d46ab58ba627d94b1cbf3ca8138d31db3246f8c5",
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/writer_cache_hits_20261011/overlay/process_metrics.hpp": "c0825d4d408a4c40d641d1d572e8c143988fb0abf2ae70c44d56d247fc0dbd79",
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/writer_cache_hits_20261011/overlay/tunable/timing.hpp": "ca262b9ef51df9b412f0e35c1f12008b546400393decb6ed3c7734ff55e4b178",
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/writer_cache_hits_20261011/overlay/tunable/performance_test.hpp": "80a679d8b4a9cadd3c933b465f2cdabc0fb9fac9b598af571397bef7e41c4037",
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/writer_cache_hits_20261011/overlay/tunable/adapters.hpp": "0af8e401f19a8ea8d2fe9615346c81bdc668db8a8ecbe54dbe29d2a1609b2785",
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/writer_cache_hits_20261011/overlay/tunable/config.hpp": "8807552e787b72d0fd9f224b268459e296cd8dbfc6ee51d85cd349be9dc61706",
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/writer_cache_hits_20261011/overlay/tunable/publication_plan.hpp": "c8d5ac56692a6d4954661f524cb5f5a81fbc192dee9ad6c565c44ddb33811ca5",
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/writer_cache_hits_20261011/overlay/tunable/producer_pmu.hpp": "74a210dce499275e240d6f55589a0b7ec2060a3e730c37705f9953c2c88105c7",
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/writer_cache_hits_20261011/overlay/tunable/parameters.hpp": "4a37ffaa2e533ee46687bd903e25d913f6440fc6eacf28b07e85c6a796dc7c29",
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/writer_cache_hits_20261011/overlay/tunable/main.cpp": "a1e8640254aac11b5e7e1fdc8c3caf61f9c46a2e9310bc60ddcdea965ef8effd",
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/tool_install_20261011/mperf/apple_perf_stat.c": "64f2fc7ea5345d3e6ed6b0f0677fdfea3a95a4b1041f2c02b3f8818204efa3c0",
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/writer_cache_hits_20261011/summarize_cache_hits.py": "459debc750b7d655dc7a5b896af2d0f6ad5a900780a8f48a04a29a765bcb5b0b",
    "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/per_writer_block_size_20261011/build/libTwoBufQueue.a": "9944f7f0e055b73e02a733f500800272bb8b39485dfd2c2db053966560983798"
  },
  "event_database_sha256": "3cbd15ac7b4c5a2460ed23c707c6c99f935909eab1d8b60c6eeb1c24c69b1640",
  "event_definitions": {
    "load": {
      "group": "load",
      "events": [
        "cycles",
        "instructions",
        "INST_INT_LD",
        "INST_SIMD_LD",
        "L1D_CACHE_MISS_LD_NONSPEC",
        "LD_UNIT_UOP",
        "L1D_CACHE_MISS_LD",
        "L1D_CACHE_WRITEBACK"
      ],
      "definitions": {
        "INST_INT_LD": {
          "counters_mask": 224,
          "number": 149,
          "description": "Retired load Integer Unit instructions"
        },
        "INST_SIMD_LD": {
          "counters_mask": 224,
          "number": 152,
          "description": "Retired load Advanced SIMD and FP Unit instructions"
        },
        "L1D_CACHE_MISS_LD_NONSPEC": {
          "counters_mask": 224,
          "number": 191,
          "description": "Retired loads that missed in the L1 Data Cache"
        },
        "LD_UNIT_UOP": {
          "number": 1446,
          "description": "Uops that flowed through the Load Unit"
        },
        "L1D_CACHE_MISS_LD": {
          "number": 1443,
          "description": "Loads that missed the L1 Data Cache"
        },
        "L1D_CACHE_WRITEBACK": {
          "number": 1448,
          "description": "Dirty cache lines written back from the L1D Cache toward the Shared L2 Cache"
        }
      }
    },
    "store": {
      "group": "store",
      "events": [
        "cycles",
        "instructions",
        "INST_INT_ST",
        "INST_SIMD_ST",
        "L1D_CACHE_MISS_ST_NONSPEC",
        "ST_UNIT_UOP",
        "L1D_CACHE_MISS_ST",
        "L1D_CACHE_WRITEBACK"
      ],
      "definitions": {
        "INST_INT_ST": {
          "counters_mask": 128,
          "number": 150,
          "description": "Retired store Integer Unit instructions"
        },
        "INST_SIMD_ST": {
          "counters_mask": 224,
          "number": 153,
          "description": "Retired store Advanced SIMD and FP Unit instructions"
        },
        "L1D_CACHE_MISS_ST_NONSPEC": {
          "counters_mask": 224,
          "number": 192,
          "description": "Retired stores that missed in the L1 Data Cache"
        },
        "ST_UNIT_UOP": {
          "number": 1447,
          "description": "Uops that flowed through the Store Unit"
        },
        "L1D_CACHE_MISS_ST": {
          "number": 1442,
          "description": "Stores that missed the L1 Data Cache"
        },
        "L1D_CACHE_WRITEBACK": {
          "number": 1448,
          "description": "Dirty cache lines written back from the L1D Cache toward the Shared L2 Cache"
        }
      }
    }
  },
  "completed_measurements": 420,
  "full_validation_runs": 42,
  "calibration_runs": 36,
  "elapsed_seconds": 131.65923725,
  "power_after": {
    "command": [
      "pmset",
      "-g",
      "batt"
    ],
    "exit_code": 0,
    "stdout": "Now drawing from 'AC Power'\n -InternalBattery-0 (id=6226019)\t100%; charged; 0:00 remaining present: true\n",
    "stderr": ""
  },
  "memory_after": {
    "vm_stat": {
      "command": [
        "vm_stat"
      ],
      "exit_code": 0,
      "stdout": "Mach Virtual Memory Statistics: (page size of 16384 bytes)\nPages free:                                5770.\nPages active:                             77895.\nPages inactive:                           75895.\nPages speculative:                         2860.\nPages throttled:                              0.\nPages wired down:                        134689.\nPages purgeable:                           5557.\n\"Translation faults\":                4546032688.\nPages copy-on-write:                   50908449.\nPages zero filled:                   1723470221.\nPages reactivated:                   1604136184.\nPages purged:                         617908391.\nFile-backed pages:                        48624.\nAnonymous pages:                         108026.\nPages stored in compressor:             1298585.\nPages occupied by compressor:            175216.\nDecompressions:                      2185003729.\nCompressions:                        2467342088.\nPageins:                              180893962.\nPageouts:                               1559483.\nSwapins:                               59852490.\nSwapouts:                              62254796.\n",
      "stderr": ""
    },
    "swap": {
      "command": [
        "sysctl",
        "vm.swapusage"
      ],
      "exit_code": 0,
      "stdout": "vm.swapusage: total = 11264.00M  used = 10102.50M  free = 1161.50M  (encrypted)\n",
      "stderr": ""
    }
  }
}
```

计数器接口参考：[mperf](https://github.com/tmcgilchrist/mperf)、[KPC 当前线程计数示例](https://gist.github.com/ibireme/173517c208c7dc333ba962c1f0d67d12)。事件定义取自本机 `/usr/share/kpep/as1.plist`。
