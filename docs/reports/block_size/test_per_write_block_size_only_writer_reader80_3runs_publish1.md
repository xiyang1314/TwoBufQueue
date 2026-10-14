TwoBufQueue · 单线程 Writer · 1

# 单线程 Writer：块大小与发布策略

32 B 普通 write 每条 release 发布 · publish_count = 1 3 次平均 高 QoS，未硬绑核

每项 10,000,000 条，预热 50,000 条。writer 池容量上限 2 GiB，初始 2 块。 只实例化真实 Writer、WriterPool 和 Writer 所需的交接队列；没有读线程、第二层池、第二层复制或业务回调。

沿用现有普通 write 的预生成输入和逐条发送循环：32 字节全部随序号变化，数据准备、初始预触碰和预热不计入写入时间。 块满后，Writer 按原代码取得新块并交接旧块；现有通知在当前写线程内直接确认完成记录并归还旧块。自然换块、发布、取块、归还和深度统计均计时；不在每条消息后额外检查换块。

两种策略都沿用 reader 实际总容量 80% 的分轮上限；本测试仅使用同配置 reader 容量作为计算参考，不创建 reader 池。 显式轮容量覆盖值为 0 B（0 B 表示自动计算）。预热后都交接尾块，从无当前块状态开始正式测量。 每轮计时结束后 finish_block 并立即归还尾块，尾部交接不计时，下一轮申请块仍计时。正式运行不读取消息负载；每个块大小、每种策略另有独立进程校验全部字节和 FIFO 顺序。 吞吐是单线程写入与即时归还的成绩，不是完整 TwoBufQueue 流水线吞吐。

吞吐最高：2 KiB，378.696 M条/s；p50 最低：2 KiB，2.444 ns。 [查看另一种发布策略](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_write_block_size_only_writer_reader80_3runs_publish_max.html)

## 写入性能

p50/p99/p999：每组 1,000 次写入的平均耗时分位数，再对 3 次运行取平均；不是单条端到端延时。 不扣除计时器开销。吞吐与延时分别运行、分别汇总，全部重复保存在原始数据中。

| writer 单块 | M条/s | GB/s | p50 ns | p99 ns | p999 ns | 吞吐范围 M条/s | 峰值 RSS MiB |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 KiB | 332.279 | 10.633 | 2.819 | 4.583 | 7.125 | 317.401–342.272 | 498.6 |
| 2 KiB | 378.696 | 12.118 | 2.444 | 3.986 | 9.667 | 338.392–399.191 | 402.5 |
| 4 KiB | 100.245 | 3.208 | 9.903 | 17.403 | 36.833 | 92.902–105.088 | 354.5 |
| 8 KiB | 65.553 | 2.098 | 16.444 | 25.986 | 37.097 | 64.520–67.374 | 330.5 |
| 16 KiB | 44.369 | 1.420 | 23.319 | 35.361 | 44.208 | 43.351–45.420 | 318.5 |
| 32 KiB | 34.101 | 1.091 | 29.847 | 40.653 | 45.819 | 33.344–35.192 | 312.5 |
| 64 KiB | 30.061 | 0.962 | 34.931 | 48.847 | 67.069 | 29.156–30.739 | 309.6 |
| 128 KiB | 28.063 | 0.898 | 37.417 | 60.250 | 105.556 | 27.455–29.025 | 308.3 |
| 256 KiB | 25.332 | 0.811 | 40.292 | 50.347 | 64.000 | 24.824–25.616 | 307.7 |
| 512 KiB | 25.811 | 0.826 | 40.222 | 52.458 | 63.583 | 25.407–26.072 | 307.9 |
| 1 MiB | 24.544 | 0.785 | 45.278 | 57.083 | 61.986 | 23.980–24.957 | 308.6 |
| 2 MiB | 24.877 | 0.796 | 41.875 | 53.875 | 59.694 | 23.687–26.725 | 310.6 |
| 4 MiB | 23.584 | 0.755 | 41.181 | 55.306 | 62.722 | 23.482–23.767 | 314.5 |
| 8 MiB | 24.855 | 0.795 | 40.819 | 51.389 | 65.833 | 23.441–25.923 | 322.5 |
| 16 MiB | 24.467 | 0.783 | 41.403 | 51.319 | 56.528 | 23.534–26.143 | 338.4 |
| 32 MiB | 24.944 | 0.798 | 38.542 | 51.958 | 67.542 | 24.096–25.569 | 370.6 |
| 64 MiB | 26.561 | 0.850 | 40.708 | 49.500 | 61.153 | 25.188–28.439 | 434.5 |
| 128 MiB | 25.862 | 0.828 | 37.486 | 48.472 | 60.236 | 23.249–28.950 | 562.5 |
| 256 MiB | 25.561 | 0.818 | 33.833 | 84.833 | 705.153 | 22.858–28.142 | 799.7 |
| 512 MiB | 26.387 | 0.844 | 34.292 | 81.222 | 446.750 | 24.689–28.106 | 751.5 |
| 1 GiB | 25.987 | 0.832 | 35.806 | 79.986 | 441.319 | 24.754–26.843 | 895.7 |

## 取块、换块与池深度

取块次数、自然换块次数、完成块数是每次吞吐运行的平均值。完成块数包含轮末尾块，自然换块不包含轮末 finish_block。 池最大值取 3 次最大；均值按全部成功取块样本加权，采样发生在新块借出之后、旧块归还之前。因此小块的取块平均占用会接近 2，归还完成后实际只剩当前块被借出。 两种策略的取块和换块次数必须一致。已分配容量包括空闲块，与实际 RSS 分开统计。

| writer 单块 | 取块/次 | 自然换块/次 | 完成块/次 | 最大占用块数 | 平均占用块数 | 最大分配块数 | 平均分配块数 | 最大分配 MiB | 背压/次 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 KiB | 312,500 | 312,499 | 312,500 | 2 | 2.000 | 2 | 2.000 | 0.00 | 0 |
| 2 KiB | 156,250 | 156,249 | 156,250 | 2 | 2.000 | 2 | 2.000 | 0.00 | 0 |
| 4 KiB | 78,125 | 78,124 | 78,125 | 2 | 2.000 | 2 | 2.000 | 0.01 | 0 |
| 8 KiB | 39,063 | 39,062 | 39,063 | 2 | 2.000 | 2 | 2.000 | 0.02 | 0 |
| 16 KiB | 19,532 | 19,531 | 19,532 | 2 | 2.000 | 2 | 2.000 | 0.03 | 0 |
| 32 KiB | 9,766 | 9,765 | 9,766 | 2 | 2.000 | 2 | 2.000 | 0.06 | 0 |
| 64 KiB | 4,883 | 4,882 | 4,883 | 2 | 2.000 | 2 | 2.000 | 0.12 | 0 |
| 128 KiB | 2,442 | 2,441 | 2,442 | 2 | 2.000 | 2 | 2.000 | 0.25 | 0 |
| 256 KiB | 1,221 | 1,220 | 1,221 | 2 | 1.999 | 2 | 2.000 | 0.50 | 0 |
| 512 KiB | 611 | 610 | 611 | 2 | 1.998 | 2 | 2.000 | 1.00 | 0 |
| 1 MiB | 306 | 305 | 306 | 2 | 1.997 | 2 | 2.000 | 2.00 | 0 |
| 2 MiB | 153 | 152 | 153 | 2 | 1.993 | 2 | 2.000 | 4.00 | 0 |
| 4 MiB | 77 | 76 | 77 | 2 | 1.987 | 2 | 2.000 | 8.00 | 0 |
| 8 MiB | 39 | 38 | 39 | 2 | 1.974 | 2 | 2.000 | 16.00 | 0 |
| 16 MiB | 20 | 19 | 20 | 2 | 1.950 | 2 | 2.000 | 32.00 | 0 |
| 32 MiB | 10 | 9 | 10 | 2 | 1.900 | 2 | 2.000 | 64.00 | 0 |
| 64 MiB | 5 | 4 | 5 | 2 | 1.800 | 2 | 2.000 | 128.00 | 0 |
| 128 MiB | 3 | 2 | 3 | 2 | 1.667 | 2 | 2.000 | 256.00 | 0 |
| 256 MiB | 2 | 1 | 2 | 2 | 1.500 | 2 | 2.000 | 512.00 | 0 |
| 512 MiB | 1 | 0 | 1 | 1 | 1.000 | 2 | 2.000 | 1,024.00 | 0 |
| 1 GiB | 1 | 0 | 1 | 1 | 1.000 | 2 | 2.000 | 2,048.00 | 0 |

### 环境、命令与源文件指纹



```
{
  "timestamp": "2026-10-11T17:47:37.986267+08:00",
  "platform": "macOS-14.4.1-arm64-arm-64bit",
  "machine": "arm64",
  "build": {
    "optimized": true,
    "sanitized": false,
    "pool_depth_instrumented": true,
    "single_thread": true,
    "payload_bytes": 32,
    "supported_publish_counts": [
      1,
      4294967295
    ]
  },
  "binary": "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/per_write_block_size_only_writer_20261011/build/test_per_write_block_size_only_writer",
  "binary_sha256": "1ddfc77351f06b329b6c5de6de5189433cac321ccd32f9ac42373e33a122c204",
  "settings": {
    "min_block_bytes": 1024,
    "max_block_bytes": 1073741824,
    "writer_cache_bytes": 2147483648,
    "initial_pool_blocks": 2,
    "messages": 10000000,
    "warmup_messages": 50000,
    "repetitions": 3,
    "round_storage_bytes": 0,
    "reader_cache_bytes": 2147483648,
    "reader_block_multiplier": 10,
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
      "stdout": "Mach Virtual Memory Statistics: (page size of 16384 bytes)\nPages free:                               39005.\nPages active:                             72631.\nPages inactive:                           68908.\nPages speculative:                         3353.\nPages throttled:                              0.\nPages wired down:                        125216.\nPages purgeable:                             28.\n\"Translation faults\":                4674606068.\nPages copy-on-write:                   51822757.\nPages zero filled:                   1763961090.\nPages reactivated:                   1648517283.\nPages purged:                         632808868.\nFile-backed pages:                        52498.\nAnonymous pages:                          92394.\nPages stored in compressor:             1345803.\nPages occupied by compressor:            162849.\nDecompressions:                      2256688471.\nCompressions:                        2548912733.\nPageins:                              183953656.\nPageouts:                               1588458.\nSwapins:                               63698875.\nSwapouts:                              66229105.\n",
      "stderr": ""
    },
    "swap": {
      "command": [
        "sysctl",
        "vm.swapusage"
      ],
      "exit_code": 1,
      "stdout": "",
      "stderr": "sysctl: sysctl fmt -1 1024 1: Operation not permitted\n"
    }
  },
  "source_sha256": {
    "CMakeLists.txt": "bb22c7367105eb6e12a527787c8f986eee63581e8bbe0c46710755cb37d0e00b",
    "benchmarks/spsc/spsc.cmake": "3a506a70c22f1bb95cb1c2471747857692f68247416ac02417f2345124cde788",
    "benchmarks/block_size/test_per_write_block_size_only_writer.py": "d1994a2316dc0c91e7fbb33498c3cd3418142dc870bd01cd01f731b88c4bd893",
    "benchmarks/block_size/test_per_writer_block_size.py": "4cd2815bd9da54161f11829994c2e86f906cdcbe9868360acf82cee426abb049",
    "benchmarks/block_size/test_per_write_block_size_only_writer.cpp": "cf5843ff0959c49a44ee069f5ec312ba81faf08b62a6406b579bc99de9c494a2",
    "benchmarks/block_size/block_size_round_budget.hpp": "7e8968986a8c3dcdc419a38ce0a045aa3e31a289719cf90c2526b9953ad1faa9",
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
  "measurement": "single-thread ordinary 32 B Writer, immediate completed-block recycle, no payload read during measurement",
  "publish_counts": [
    1,
    4294967295
  ],
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
      "stdout": "Mach Virtual Memory Statistics: (page size of 16384 bytes)\nPages free:                               22098.\nPages active:                             85607.\nPages inactive:                           81275.\nPages speculative:                         4075.\nPages throttled:                              0.\nPages wired down:                        125947.\nPages purgeable:                           4283.\n\"Translation faults\":                4686961780.\nPages copy-on-write:                   51883345.\nPages zero filled:                   1774122626.\nPages reactivated:                   1649723655.\nPages purged:                         633035972.\nFile-backed pages:                        51873.\nAnonymous pages:                         119084.\nPages stored in compressor:             1316341.\nPages occupied by compressor:            153550.\nDecompressions:                      2258391587.\nCompressions:                        2552983755.\nPageins:                              184027014.\nPageouts:                               1588893.\nSwapins:                               63764790.\nSwapouts:                              66291725.\n",
      "stderr": ""
    },
    "swap": {
      "command": [
        "sysctl",
        "vm.swapusage"
      ],
      "exit_code": 1,
      "stdout": "",
      "stderr": "sysctl: sysctl fmt -1 1024 1: Operation not permitted\n"
    }
  },
  "elapsed_seconds": 71.254476875,
  "completed_runs": 294,
  "validation_runs": 42
}
```
