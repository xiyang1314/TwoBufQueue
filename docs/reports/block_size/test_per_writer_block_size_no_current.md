TwoBufQueue · writer block size 遍历

# writer 块大小与热路径性能

32 B 仅交接块时发布 · UINT32_MAX 不扫描当前块 回调循环 0 5 次平均

每项发送 10,000,000 条。reader 单块始终为 writer 的 10 倍，初始各 2 块（受池上限约束），随后按需增长。 writer 容量上限 2 GiB；reader 基础上限 2 GiB，不足一个 reader 块时提高到该块大小。

沿用 PerformanceTest 的数据准备、普通 write、两层复制、独立回调线程、分轮发送及排空逻辑。数据准备与轮间排空不计入写入吞吐；统计换块的开销包含在测量中。 未硬绑核。采用高 QoS 调度偏好。没有添加目标读回或额外清零优化。

关闭 current 扫描时，沿用框架在每轮计时结束后 finish_block 交接尾块并排空；交接与排空时间不计入写入吞吐，下一轮申请新块仍计时。发布次数见上方标签。

吞吐最高：8 MiB；p50 最低：32 MiB。所有重复均保留，未挑选最快一次。

环境说明：测试前后系统整体 Swapins 累计增加 421.4 MiB，Swapouts 累计增加 650.9 MiB。这些是系统计数，不能归因于某个测试进程；本次成绩包含当前系统负载的影响。

## 写入性能

p50/p99/p999 是每组 1,000 次成功写入的平均耗时分位数，再对各次运行取平均；不是单条端到端延时。 回调/写入比为每轮首条至末条回调的时间与写入时间之比，包含分发和等待。RSS 是进程实际驻留内存峰值。

| writer 单块 | reader 单块 | M条/s | GB/s | p50 ns | p99 ns | p999 ns | 吞吐范围 M条/s | 回调/写入比 | 峰值 RSS MiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 KiB | 10 KiB | 107.768 | 3.449 | 8.617 | 21.358 | 48.642 | 93.894–115.488 | 1.01 | 232.2 |
| 2 KiB | 20 KiB | 194.999 | 6.240 | 5.008 | 10.275 | 23.000 | 185.409–201.717 | 1.00 | 122.2 |
| 4 KiB | 40 KiB | 256.933 | 8.222 | 3.733 | 12.250 | 55.658 | 237.449–278.942 | 1.17 | 83.2 |
| 8 KiB | 80 KiB | 332.605 | 10.643 | 2.925 | 6.000 | 18.025 | 327.830–338.239 | 1.27 | 58.6 |
| 16 KiB | 160 KiB | 389.965 | 12.479 | 2.483 | 6.025 | 23.025 | 353.412–403.400 | 1.45 | 50.6 |
| 32 KiB | 320 KiB | 396.751 | 12.696 | 2.417 | 4.483 | 14.150 | 384.606–408.001 | 1.40 | 40.5 |
| 64 KiB | 640 KiB | 385.673 | 12.342 | 2.333 | 5.850 | 18.250 | 315.985–407.156 | 1.41 | 38.9 |
| 128 KiB | 1.25 MiB | 421.911 | 13.501 | 2.208 | 4.733 | 13.433 | 396.458–439.859 | 1.50 | 39.0 |
| 256 KiB | 2.5 MiB | 407.318 | 13.034 | 2.167 | 4.892 | 16.742 | 306.330–450.118 | 1.53 | 41.0 |
| 512 KiB | 5 MiB | 447.387 | 14.316 | 2.175 | 5.575 | 14.408 | 408.070–465.969 | 1.52 | 25.2 |
| 1 MiB | 10 MiB | 440.305 | 14.090 | 2.133 | 4.192 | 11.233 | 402.801–468.652 | 1.50 | 27.0 |
| 2 MiB | 20 MiB | 476.232 | 15.239 | 2.133 | 5.442 | 16.175 | 469.695–484.334 | 1.56 | 23.8 |
| 4 MiB | 40 MiB | 475.018 | 15.201 | 2.092 | 3.625 | 10.108 | 407.485–497.033 | 1.60 | 24.4 |
| 8 MiB | 80 MiB | 521.301 | 16.682 | 2.100 | 3.108 | 8.917 | 515.092–525.084 | 1.70 | 30.4 |
| 16 MiB | 160 MiB | 500.838 | 16.027 | 2.092 | 3.267 | 9.758 | 445.030–539.957 | 1.65 | 46.5 |
| 32 MiB | 320 MiB | 506.786 | 16.217 | 2.075 | 3.192 | 8.633 | 488.630–523.471 | 1.64 | 78.5 |
| 64 MiB | 640 MiB | 510.605 | 16.339 | 2.083 | 2.775 | 6.992 | 495.638–523.598 | 1.65 | 142.5 |
| 128 MiB | 1.25 GiB | 468.100 | 14.979 | 2.075 | 2.933 | 8.150 | 371.883–536.946 | 1.63 | 270.5 |
| 256 MiB | 2.5 GiB | 493.755 | 15.800 | 2.092 | 5.358 | 27.050 | 485.004–500.966 | 1.61 | 490.8 |
| 512 MiB | 5 GiB | 465.885 | 14.908 | 2.083 | 3.950 | 21.083 | 419.599–490.880 | 1.57 | 643.5 |
| 1 GiB | 10 GiB | 465.844 | 14.907 | 2.083 | 4.875 | 34.592 | 393.520–510.878 | 1.58 | 618.6 |

## 占用深度：已借出、尚未归还的块

每次成功取新块后，在原有池锁内取得一致计数。均值按取块次数加权；最大值包含正式阶段起始快照。这里汇总吞吐测试的全部重复，延时测试的独立深度数据保存在 JSON。 没有取块样本时均值显示 —。writer/reader 两侧独立采样，不代表同一时刻的联合快照。

| writer 单块 | writer 最大块数 | writer 平均块数 | writer 最大占用 MiB | writer 样本数 | reader 最大块数 | reader 平均块数 | reader 最大占用 MiB | reader 样本数 | writer 背压/次 | 复制层背压/次 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 KiB | 4,457 | 67.305 | 4.35 | 1,562,600 | 222 | 9.008 | 2.17 | 293,146 | 0.0 | 0.0 |
| 2 KiB | 1,056 | 12.934 | 2.06 | 781,300 | 113 | 8.635 | 2.21 | 204,697 | 0.0 | 0.0 |
| 4 KiB | 591 | 11.061 | 2.31 | 390,770 | 222 | 89.044 | 8.67 | 220,653 | 0.0 | 0.0 |
| 8 KiB | 139 | 8.020 | 1.09 | 195,505 | 227 | 99.614 | 17.73 | 145,619 | 0.0 | 0.0 |
| 16 KiB | 93 | 5.555 | 1.45 | 97,755 | 168 | 72.627 | 26.25 | 79,469 | 0.0 | 0.0 |
| 32 KiB | 50 | 4.239 | 1.56 | 48,880 | 83 | 33.545 | 25.94 | 40,177 | 0.0 | 0.0 |
| 64 KiB | 58 | 3.805 | 3.62 | 24,560 | 39 | 16.181 | 24.38 | 18,165 | 0.0 | 0.0 |
| 128 KiB | 21 | 2.958 | 2.62 | 12,400 | 25 | 10.654 | 31.25 | 10,791 | 0.0 | 0.0 |
| 256 KiB | 17 | 2.602 | 4.25 | 6,200 | 16 | 6.135 | 40.00 | 5,474 | 0.0 | 0.0 |
| 512 KiB | 5 | 2.055 | 2.50 | 3,100 | 7 | 3.916 | 35.00 | 3,058 | 0.0 | 0.0 |
| 1 MiB | 5 | 2.007 | 5.00 | 1,670 | 5 | 2.842 | 50.00 | 1,645 | 0.0 | 0.0 |
| 2 MiB | 3 | 1.815 | 6.00 | 955 | 4 | 2.228 | 80.00 | 934 | 0.0 | 0.0 |
| 4 MiB | 2 | 1.500 | 8.00 | 480 | 2 | 1.499 | 80.00 | 479 | 0.0 | 0.0 |
| 8 MiB | 1 | 1.000 | 8.00 | 240 | 1 | 1.000 | 80.00 | 240 | 0.0 | 0.0 |
| 16 MiB | 1 | 1.000 | 16.00 | 240 | 1 | 1.000 | 160.00 | 240 | 0.0 | 0.0 |
| 32 MiB | 1 | 1.000 | 32.00 | 240 | 1 | 1.000 | 320.00 | 240 | 0.0 | 0.0 |
| 64 MiB | 1 | 1.000 | 64.00 | 240 | 1 | 1.000 | 640.00 | 240 | 0.0 | 0.0 |
| 128 MiB | 1 | 1.000 | 128.00 | 240 | 1 | 1.000 | 1,280.00 | 240 | 0.0 | 0.0 |
| 256 MiB | 1 | 1.000 | 256.00 | 240 | 1 | 1.000 | 2,560.00 | 240 | 0.0 | 0.0 |
| 512 MiB | 1 | 1.000 | 512.00 | 240 | 1 | 1.000 | 5,120.00 | 240 | 0.0 | 0.0 |
| 1 GiB | 1 | 1.000 | 1,024.00 | 240 | 1 | 1.000 | 10,240.00 | 240 | 0.0 | 0.0 |

## 已分配块数与容量

已分配包括借出和池内空闲块。下面的容量是块数 × 单块字节数，包含尚未触碰的虚拟内存，不等于实际 RSS。 平均值仍按成功取块次数加权。

| writer 单块 | writer 最大分配块数 | writer 平均分配块数 | writer 最大分配 MiB | reader 最大分配块数 | reader 平均分配块数 | reader 最大分配 MiB | reader 容量上限 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 KiB | 4,457 | 1,074.180 | 4.35 | 223 | 99.455 | 2.18 | 2 GiB |
| 2 KiB | 1,056 | 373.326 | 2.06 | 113 | 65.398 | 2.21 | 2 GiB |
| 4 KiB | 591 | 154.786 | 2.31 | 223 | 168.190 | 8.71 | 2 GiB |
| 8 KiB | 139 | 97.273 | 1.09 | 228 | 176.700 | 17.81 | 2 GiB |
| 16 KiB | 93 | 48.851 | 1.45 | 169 | 142.713 | 26.41 | 2 GiB |
| 32 KiB | 50 | 29.971 | 1.56 | 83 | 68.951 | 25.94 | 2 GiB |
| 64 KiB | 58 | 20.948 | 3.62 | 39 | 35.320 | 24.38 | 2 GiB |
| 128 KiB | 21 | 11.372 | 2.62 | 25 | 21.876 | 31.25 | 2 GiB |
| 256 KiB | 17 | 7.473 | 4.25 | 17 | 12.402 | 42.50 | 2 GiB |
| 512 KiB | 5 | 3.545 | 2.50 | 8 | 6.989 | 40.00 | 2 GiB |
| 1 MiB | 5 | 3.808 | 5.00 | 5 | 4.899 | 50.00 | 2 GiB |
| 2 MiB | 3 | 2.987 | 6.00 | 4 | 3.726 | 80.00 | 2 GiB |
| 4 MiB | 2 | 2.000 | 8.00 | 2 | 2.000 | 80.00 | 2 GiB |
| 8 MiB | 2 | 2.000 | 16.00 | 2 | 2.000 | 160.00 | 2 GiB |
| 16 MiB | 2 | 2.000 | 32.00 | 2 | 2.000 | 320.00 | 2 GiB |
| 32 MiB | 2 | 2.000 | 64.00 | 2 | 2.000 | 640.00 | 2 GiB |
| 64 MiB | 2 | 2.000 | 128.00 | 2 | 2.000 | 1,280.00 | 2 GiB |
| 128 MiB | 2 | 2.000 | 256.00 | 1 | 1.000 | 1,280.00 | 2 GiB |
| 256 MiB | 2 | 2.000 | 512.00 | 1 | 1.000 | 2,560.00 | 2.5 GiB |
| 512 MiB | 2 | 2.000 | 1,024.00 | 1 | 1.000 | 5,120.00 | 5 GiB |
| 1 GiB | 2 | 2.000 | 2,048.00 | 1 | 1.000 | 10,240.00 | 10 GiB |

### 环境、二进制和源文件指纹



```
{
  "timestamp": "2026-10-11T15:38:05.384730+08:00",
  "platform": "macOS-14.4.1-arm64-arm-64bit",
  "machine": "arm64",
  "build": {
    "compiler": "Homebrew Clang 18.1.8",
    "optimized": true,
    "sanitized": false,
    "pool_depth_instrumented": true,
    "payload_bytes": 32,
    "supported_publish_counts": [
      1,
      4294967295
    ]
  },
  "binary": "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/per_writer_block_size_20261011/build/test_per_writer_block_size",
  "binary_sha256": "1af289313bc43d62bc83f65fc3fbe22766207e14ff42773a6f8bc0985721c2dd",
  "settings": {
    "min_block_bytes": 1024,
    "max_block_bytes": 1073741824,
    "reader_block_multiplier": 10,
    "writer_cache_bytes": 2147483648,
    "reader_cache_bytes": 2147483648,
    "initial_pool_blocks": 2,
    "messages": 10000000,
    "warmup_messages": 50000,
    "callback_iterations": 0,
    "is_scan_current": false,
    "publish_count": 4294967295,
    "repetitions": 5,
    "round_storage_bytes": 6710886,
    "latency_batch_size": 1000,
    "producer_cpu": -1,
    "producer_qos": "user_interactive",
    "timeout_seconds": 180,
    "order_seed": 1729
  },
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
      "stdout": "Mach Virtual Memory Statistics: (page size of 16384 bytes)\nPages free:                                4918.\nPages active:                             77628.\nPages inactive:                           74038.\nPages speculative:                         2569.\nPages throttled:                              0.\nPages wired down:                        129201.\nPages purgeable:                              2.\n\"Translation faults\":                4494444955.\nPages copy-on-write:                   50466600.\nPages zero filled:                   1703970046.\nPages reactivated:                   1588138660.\nPages purged:                         612297986.\nFile-backed pages:                        41257.\nAnonymous pages:                         112978.\nPages stored in compressor:             1297045.\nPages occupied by compressor:            184052.\nDecompressions:                      2158562885.\nCompressions:                        2433714226.\nPageins:                              179473542.\nPageouts:                               1548646.\nSwapins:                               58364357.\nSwapouts:                              60708324.\n",
      "stderr": ""
    },
    "swap": {
      "command": [
        "sysctl",
        "vm.swapusage"
      ],
      "exit_code": 0,
      "stdout": "vm.swapusage: total = 10240.00M  used = 9245.00M  free = 995.00M  (encrypted)\n",
      "stderr": ""
    }
  },
  "source_sha256": {
    "CMakeLists.txt": "bb22c7367105eb6e12a527787c8f986eee63581e8bbe0c46710755cb37d0e00b",
    "benchmarks/spsc/spsc.cmake": "bd9d90a5c5d7805e7213dac0e86b371232a676401ac560259aec77abb57b3543",
    "benchmarks/block_size/test_per_writer_block_size.py": "0f1eaf6e977e98378cc4be006f73ab22d98984fdbb6aabdf305b408bdd239976",
    "benchmarks/block_size/test_per_writer_block_size.cpp": "90d7ba653e84c288c96aa3dedb2b043f31e5947fcd0d87810137e1db3d6e16ef",
    "src/two_buf_queue.hpp": "cabc2ab36d4c9d3fdfd3a3b6b8b2302824db30aaf8fafa7144fd2e8576de4039",
    "src/write/seq_writer.cpp": "8128732abe1e41c491630a2f65d15858d59bd4ef92dcf45c854f169855003982",
    "src/write/make_queue.hpp": "808415585792bbeba881b0af7beb6c60e9bf16d21b742a7f163775834ad478f7",
    "src/write/writer.hpp": "f850ed8e1d19eb6ef2dc46b22358fe550c843b96cfc7bcee672f908c6187a9cd",
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
    "src/data/data_batch.hpp": "8673ec49ed45ff511335cd0e853a72875d649b461376613ca5618ac355378bf9",
    "include/TwoBufQueue.hpp": "990e5cb0946b060c286839a431bb0252f067268425add0b258cc8511e6fcb8d4",
    "benchmarks/tunable/timing.hpp": "ca262b9ef51df9b412f0e35c1f12008b546400393decb6ed3c7734ff55e4b178",
    "benchmarks/tunable/performance_test.hpp": "dafcf50ea6fb0f6808d50f5d509c835102376cc0743b0c2353e0f379a7ecfb64",
    "benchmarks/tunable/adapters.hpp": "0af8e401f19a8ea8d2fe9615346c81bdc668db8a8ecbe54dbe29d2a1609b2785",
    "benchmarks/tunable/config.hpp": "8807552e787b72d0fd9f224b268459e296cd8dbfc6ee51d85cd349be9dc61706",
    "benchmarks/tunable/publication_plan.hpp": "c8d5ac56692a6d4954661f524cb5f5a81fbc192dee9ad6c565c44ddb33811ca5",
    "benchmarks/tunable/parameters.hpp": "4a37ffaa2e533ee46687bd903e25d913f6440fc6eacf28b07e85c6a796dc7c29",
    "benchmarks/tunable/main.cpp": "a1e8640254aac11b5e7e1fdc8c3caf61f9c46a2e9310bc60ddcdea965ef8effd"
  },
  "dependencies": {
    "rigtorp": {
      "repository": "rigtorp/SPSCQueue",
      "commit": "1053918dbd251fbff69b24ef27fa5d51c29ec2af",
      "files": [
        {
          "path": "include/rigtorp/SPSCQueue.h",
          "url": "https://raw.githubusercontent.com/rigtorp/SPSCQueue/1053918dbd251fbff69b24ef27fa5d51c29ec2af/include/rigtorp/SPSCQueue.h",
          "sha256": "1e631ec9e8ba4955da5cac116620815055f7da0ea936bfdb3036c4e87bc8a6e8"
        },
        {
          "path": "LICENSE",
          "url": "https://raw.githubusercontent.com/rigtorp/SPSCQueue/1053918dbd251fbff69b24ef27fa5d51c29ec2af/LICENSE",
          "sha256": "d0887259b1e1791964efe737886d5742bf025991b3f4db6b05eefbdbaa4d7224"
        }
      ]
    },
    "atomic_queue": {
      "repository": "max0x7ba/atomic_queue",
      "commit": "74fb62f86e6c3b0db824932a5c5e4bfb0125e2cc",
      "files": [
        {
          "path": "include/atomic_queue/atomic_queue.h",
          "url": "https://raw.githubusercontent.com/max0x7ba/atomic_queue/74fb62f86e6c3b0db824932a5c5e4bfb0125e2cc/include/atomic_queue/atomic_queue.h",
          "sha256": "76188465b20c706b37e1c80948b0d0c0a08e9e5144eb536c32e33bd035db0161"
        },
        {
          "path": "include/atomic_queue/defs.h",
          "url": "https://raw.githubusercontent.com/max0x7ba/atomic_queue/74fb62f86e6c3b0db824932a5c5e4bfb0125e2cc/include/atomic_queue/defs.h",
          "sha256": "df0085237bfd1b5effeeae14a94b28512948d2f647142c9fc6923aa8aabfd746"
        },
        {
          "path": "LICENSE",
          "url": "https://raw.githubusercontent.com/max0x7ba/atomic_queue/74fb62f86e6c3b0db824932a5c5e4bfb0125e2cc/LICENSE",
          "sha256": "22e4fce4f072000d810546875f26ad1e8b23fcb942fe0a00c85602eb3626c1bb"
        }
      ]
    },
    "readerwriterqueue": {
      "repository": "cameron314/readerwriterqueue",
      "commit": "8e7627d18c2108aca178888d88514179899a044f",
      "version": "v1.0.6",
      "files": [
        {
          "path": "readerwriterqueue.h",
          "url": "https://raw.githubusercontent.com/cameron314/readerwriterqueue/8e7627d18c2108aca178888d88514179899a044f/readerwriterqueue.h",
          "sha256": "c7abdbe90059b8b8c649d5752694bc41cfc0a8510e655513b7b8a01fce5741d6"
        },
        {
          "path": "atomicops.h",
          "url": "https://raw.githubusercontent.com/cameron314/readerwriterqueue/8e7627d18c2108aca178888d88514179899a044f/atomicops.h",
          "sha256": "e0aa232fd22d2e061830e33ff9ae53106eab08d2afda49e0e51d22f9d514804e"
        },
        {
          "path": "LICENSE.md",
          "url": "https://raw.githubusercontent.com/cameron314/readerwriterqueue/8e7627d18c2108aca178888d88514179899a044f/LICENSE.md",
          "sha256": "018fb474bdf10bc068f5e3f2c61e18df8326d60b46db3eb017b4411eda189afc"
        }
      ]
    }
  },
  "measurement": "32 B ordinary write, publish_count=4294967295, completed blocks only, dynamic pools",
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
      "stdout": "Mach Virtual Memory Statistics: (page size of 16384 bytes)\nPages free:                               16163.\nPages active:                             66409.\nPages inactive:                           65275.\nPages speculative:                         2044.\nPages throttled:                              0.\nPages wired down:                        129600.\nPages purgeable:                           4098.\n\"Translation faults\":                4498968208.\nPages copy-on-write:                   50509251.\nPages zero filled:                   1707591852.\nPages reactivated:                   1588572391.\nPages purged:                         612369091.\nFile-backed pages:                        47219.\nAnonymous pages:                          86509.\nPages stored in compressor:             1322197.\nPages occupied by compressor:            192437.\nDecompressions:                      2159179107.\nCompressions:                        2436243234.\nPageins:                              179504089.\nPageouts:                               1548861.\nSwapins:                               58391329.\nSwapouts:                              60749984.\n",
      "stderr": ""
    },
    "swap": {
      "command": [
        "sysctl",
        "vm.swapusage"
      ],
      "exit_code": 0,
      "stdout": "vm.swapusage: total = 10240.00M  used = 9471.94M  free = 768.06M  (encrypted)\n",
      "stderr": ""
    }
  },
  "elapsed_seconds": 23.622729708,
  "completed_runs": 231
}
```
