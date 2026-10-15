TwoBufQueue · 单线程 Writer · 1

# 单线程 Writer：块大小与发布策略

32 B 普通 write 每条 release 发布 · publish_count = 1 5 次平均 高 QoS，未硬绑核

每项 10,000,000 条，预热 50,000 条。writer 池容量上限 2 GiB，初始 2 块。 只实例化真实 Writer、WriterPool 和 Writer 所需的交接队列；没有读线程、第二层池、第二层复制或业务回调。

沿用现有普通 write 的预生成输入和逐条发送循环：32 字节全部随序号变化，数据准备、初始预触碰和预热不计入写入时间。 块满后，Writer 按原代码取得新块并交接旧块；现有通知在当前写线程内直接确认完成记录并归还旧块。自然换块、发布、取块、归还和深度统计均计时；不在每条消息后额外检查换块。

两种策略都使用相同轮容量 6.4 MiB，预热后都交接尾块，从无当前块状态开始正式测量。 每轮计时结束后 finish_block 并立即归还尾块，尾部交接不计时，下一轮申请块仍计时。正式运行不读取消息负载；每个块大小、每种策略另有独立进程校验全部字节和 FIFO 顺序。 吞吐是单线程写入与即时归还的成绩，不是完整 TwoBufQueue 流水线吞吐。

吞吐最高：2 KiB，401.232 M条/s；p50 最低：2 KiB，2.442 ns。 [查看另一种发布策略](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_write_block_size_only_writer_publish_max.html)

## 写入性能

p50/p99/p999：每组 1,000 次写入的平均耗时分位数，再对五次运行取平均；不是单条端到端延时。 不扣除计时器开销。吞吐与延时分别运行、分别汇总，全部重复保存在原始数据中。

| writer 单块 | M条/s | GB/s | p50 ns | p99 ns | p999 ns | 吞吐范围 M条/s | 峰值 RSS MiB |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 KiB | 334.188 | 10.694 | 2.808 | 4.117 | 7.875 | 288.682–347.259 | 199.7 |
| 2 KiB | 401.232 | 12.839 | 2.442 | 3.175 | 7.142 | 391.798–407.040 | 103.8 |
| 4 KiB | 98.363 | 3.148 | 9.375 | 14.825 | 22.292 | 90.834–105.325 | 55.8 |
| 8 KiB | 64.295 | 2.057 | 15.733 | 25.608 | 32.550 | 61.184–68.632 | 31.7 |
| 16 KiB | 42.634 | 1.364 | 21.833 | 35.617 | 41.033 | 38.994–44.186 | 19.7 |
| 32 KiB | 32.806 | 1.050 | 30.133 | 41.408 | 47.100 | 32.044–34.188 | 13.8 |
| 64 KiB | 27.581 | 0.883 | 34.925 | 47.492 | 55.942 | 25.847–29.623 | 10.8 |
| 128 KiB | 26.564 | 0.850 | 38.783 | 49.975 | 64.642 | 26.019–27.050 | 9.5 |
| 256 KiB | 24.654 | 0.789 | 40.867 | 53.192 | 59.283 | 23.298–25.857 | 9.0 |
| 512 KiB | 24.654 | 0.789 | 43.575 | 55.467 | 62.192 | 21.919–27.081 | 9.1 |
| 1 MiB | 23.653 | 0.757 | 41.742 | 54.625 | 69.258 | 23.347–23.996 | 9.9 |
| 2 MiB | 24.581 | 0.787 | 42.533 | 56.300 | 63.417 | 23.386–27.141 | 11.8 |
| 4 MiB | 23.738 | 0.760 | 42.808 | 56.767 | 63.525 | 22.560–24.372 | 15.8 |
| 8 MiB | 22.180 | 0.710 | 42.542 | 54.625 | 73.933 | 19.787–23.688 | 23.7 |
| 16 MiB | 24.508 | 0.784 | 41.833 | 53.475 | 59.567 | 21.497–29.847 | 39.7 |
| 32 MiB | 23.454 | 0.751 | 41.950 | 55.825 | 61.442 | 21.883–25.002 | 71.7 |
| 64 MiB | 23.425 | 0.750 | 41.333 | 54.342 | 61.975 | 20.999–26.621 | 135.7 |
| 128 MiB | 23.694 | 0.758 | 44.342 | 55.600 | 65.317 | 22.688–25.321 | 263.7 |
| 256 MiB | 22.926 | 0.734 | 42.567 | 54.442 | 65.583 | 19.152–24.365 | 519.7 |
| 512 MiB | 23.730 | 0.759 | 41.725 | 52.367 | 61.492 | 22.633–25.274 | 629.3 |
| 1 GiB | 23.735 | 0.760 | 42.692 | 52.450 | 62.458 | 22.916–24.503 | 740.0 |

## 取块、换块与池深度

取块次数、自然换块次数、完成块数是每次吞吐运行的平均值。完成块数包含轮末尾块，自然换块不包含轮末 finish_block。 池最大值取五次最大；均值按全部成功取块样本加权，采样发生在新块借出之后、旧块归还之前。因此小块的取块平均占用会接近 2，归还完成后实际只剩当前块被借出。 两种策略的取块和换块次数必须一致。已分配容量包括空闲块，与实际 RSS 分开统计。

| writer 单块 | 取块/次 | 自然换块/次 | 完成块/次 | 最大占用块数 | 平均占用块数 | 最大分配块数 | 平均分配块数 | 最大分配 MiB | 背压/次 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 KiB | 312,520 | 312,472 | 312,520 | 2 | 2.000 | 2 | 2.000 | 0.00 | 0 |
| 2 KiB | 156,260 | 156,212 | 156,260 | 2 | 2.000 | 2 | 2.000 | 0.00 | 0 |
| 4 KiB | 78,154 | 78,106 | 78,154 | 2 | 1.999 | 2 | 2.000 | 0.01 | 0 |
| 8 KiB | 39,101 | 39,053 | 39,101 | 2 | 1.999 | 2 | 2.000 | 0.02 | 0 |
| 16 KiB | 19,551 | 19,503 | 19,551 | 2 | 1.998 | 2 | 2.000 | 0.03 | 0 |
| 32 KiB | 9,776 | 9,728 | 9,776 | 2 | 1.995 | 2 | 2.000 | 0.06 | 0 |
| 64 KiB | 4,912 | 4,864 | 4,912 | 2 | 1.990 | 2 | 2.000 | 0.12 | 0 |
| 128 KiB | 2,480 | 2,432 | 2,480 | 2 | 1.981 | 2 | 2.000 | 0.25 | 0 |
| 256 KiB | 1,240 | 1,192 | 1,240 | 2 | 1.961 | 2 | 2.000 | 0.50 | 0 |
| 512 KiB | 620 | 572 | 620 | 2 | 1.923 | 2 | 2.000 | 1.00 | 0 |
| 1 MiB | 334 | 286 | 334 | 2 | 1.856 | 2 | 2.000 | 2.00 | 0 |
| 2 MiB | 191 | 143 | 191 | 2 | 1.749 | 2 | 2.000 | 4.00 | 0 |
| 4 MiB | 96 | 48 | 96 | 2 | 1.500 | 2 | 2.000 | 8.00 | 0 |
| 8 MiB | 48 | 0 | 48 | 1 | 1.000 | 2 | 2.000 | 16.00 | 0 |
| 16 MiB | 48 | 0 | 48 | 1 | 1.000 | 2 | 2.000 | 32.00 | 0 |
| 32 MiB | 48 | 0 | 48 | 1 | 1.000 | 2 | 2.000 | 64.00 | 0 |
| 64 MiB | 48 | 0 | 48 | 1 | 1.000 | 2 | 2.000 | 128.00 | 0 |
| 128 MiB | 48 | 0 | 48 | 1 | 1.000 | 2 | 2.000 | 256.00 | 0 |
| 256 MiB | 48 | 0 | 48 | 1 | 1.000 | 2 | 2.000 | 512.00 | 0 |
| 512 MiB | 48 | 0 | 48 | 1 | 1.000 | 2 | 2.000 | 1,024.00 | 0 |
| 1 GiB | 48 | 0 | 48 | 1 | 1.000 | 2 | 2.000 | 2,048.00 | 0 |

### 环境、命令与源文件指纹



```
{
  "timestamp": "2026-10-11T17:00:34.419409+08:00",
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
  "binary_sha256": "f71c0971fc69c51aac50231e766b9b13210dbbaaa7f860e615cad361c66e4ead",
  "settings": {
    "min_block_bytes": 1024,
    "max_block_bytes": 1073741824,
    "writer_cache_bytes": 2147483648,
    "initial_pool_blocks": 2,
    "messages": 10000000,
    "warmup_messages": 50000,
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
      "stdout": "Mach Virtual Memory Statistics: (page size of 16384 bytes)\nPages free:                                2605.\nPages active:                             91671.\nPages inactive:                           89753.\nPages speculative:                          936.\nPages throttled:                              0.\nPages wired down:                        125835.\nPages purgeable:                           6846.\n\"Translation faults\":                4599758175.\nPages copy-on-write:                   51267688.\nPages zero filled:                   1735991296.\nPages reactivated:                   1625233904.\nPages purged:                         625349699.\nFile-backed pages:                        46351.\nAnonymous pages:                         136009.\nPages stored in compressor:             1270808.\nPages occupied by compressor:            161542.\nDecompressions:                      2218962087.\nCompressions:                        2503839588.\nPageins:                              182260795.\nPageouts:                               1573319.\nSwapins:                               61901679.\nSwapouts:                              64345338.\n",
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
    "benchmarks/block_size/test_per_write_block_size_only_writer.py": "e322d6346153442000c988bcb476d1ecbc442c008be83b81c95bd274dae6f1ab",
    "benchmarks/block_size/test_per_writer_block_size.py": "0f1eaf6e977e98378cc4be006f73ab22d98984fdbb6aabdf305b408bdd239976",
    "benchmarks/block_size/test_per_write_block_size_only_writer.cpp": "2af1429c15aefcc2527be7663c73c0a7ce071a671a4eb7ac8f26d4d422005eee",
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
    "src/base_components/live_handoff_queue/spsc_live_handoff_queue.hpp": "04322881bf4b2a0176bc6c3ef1889f8e23d4f6ab7eaa44c4053fdfe3b3ed549f",
    "src/base_components/handoff_queue/claim_queue.hpp": "e3dd6e79d3aca4cae96f619104d44763b6ae7f87a5636088731cf1805baea3ea",
    "src/base_components/handoff_queue/pooled_resource_queue.hpp": "ce12e04b0e5d7870499962e43a377c3dd33385da97bfd460b97a24bbcee2858c",
    "src/base_components/handoff_queue/handoff_reader.hpp": "d1191417435a515003bfe029d52ee9e643888ae1bc8d610b9abfa075e509dee5",
    "src/base_components/handoff_queue/queue_ticket.hpp": "3716dbd2c38a0c6cf743be62bda01d334fbfb7a7517f003fb09a82c1d16da6ee",
    "src/base_components/handoff_queue/ready_index_queue.hpp": "2db96b7e52e6e68f708c61e90f819c69b8b17cc64faab1c82b80bafc8ec0c20b",
    "src/base_components/handoff_queue/handoff_queue.hpp": "451fac983f6fb8af9feadbdcaf9e93519bfbdb831b37ae0263181b82a9912747",
    "src/base_components/handoff_queue/handoff_types.hpp": "a877583d4dffc236c5d45d97427faa36c83fd7b51d133042e9ba3690cb57cd5f",
    "src/base_components/handoff_queue/lock_free_index_queue.hpp": "62b497f6f5d70553a6fdf2b7ea0c9ca645cb4e119c9407a7b1b30ea9e9ca338d",
    "src/base_components/warm_pool/warm_pool.hpp": "cdc88f35642e4c81bfbb9072027fe56d1592180a070bef22afadd6954c0bd641",
    "src/base_components/warm_pool/intrusive_stack.hpp": "912dae641f26b79fecb3f0926aac0e8255c0c67ff0addd4c56d73388faf98bea",
    "src/base_components/warm_pool/depth_stats.hpp": "763d12709fdcf42d81789ca86ae5cdf8d16cae9ea804ffa6c13dd27a3af1aa02",
    "src/base_components/warm_pool/intrusive_queue.hpp": "0b87a1615c000ea6aab805a6ec4443240adeb13d34f218b6d7ba0ecb6aea11ec",
    "src/base_components/warm_pool/resource.hpp": "9adffca45f4e69a6056f83fda03ff0951ff32fd5816fdbf675be92ba309d82a9",
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
      "stdout": "Mach Virtual Memory Statistics: (page size of 16384 bytes)\nPages free:                               28769.\nPages active:                             57478.\nPages inactive:                           63593.\nPages speculative:                         1149.\nPages throttled:                              0.\nPages wired down:                        137272.\nPages purgeable:                            319.\n\"Translation faults\":                4608691606.\nPages copy-on-write:                   51360109.\nPages zero filled:                   1742972189.\nPages reactivated:                   1625924759.\nPages purged:                         625615008.\nFile-backed pages:                        48338.\nAnonymous pages:                          73882.\nPages stored in compressor:             1327947.\nPages occupied by compressor:            183960.\nDecompressions:                      2220447950.\nCompressions:                        2508972186.\nPageins:                              182320524.\nPageouts:                               1574035.\nSwapins:                               61942922.\nSwapouts:                              64397630.\n",
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
  "elapsed_seconds": 101.97716483299999,
  "completed_runs": 462,
  "validation_runs": 42
}
```
