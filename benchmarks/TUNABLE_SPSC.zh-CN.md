# 单 Writer 参数搜索与 SPSC 对比

这套基准使用现版完整 TwoBufQueue：`Writer → SingleReadWorker → ToReadBlock 第二层复制 → 输出队列 → 逐条消息回调`。对照为 [rigtorp/SPSCQueue](https://github.com/rigtorp/SPSCQueue)、[moodycamel ReaderWriterQueue](https://github.com/cameron314/readerwriterqueue)、[atomic_queue](https://github.com/max0x7ba/atomic_queue) 的 SPSC 模式。每条消息传完整值，不传指针，不额外加时间戳。

同一工作负载中，所有实现固定相同的正式成功写入次数、预热次数、消息长度和逐条回调计算。分别对 **吞吐 M msg/s、组内平均写入耗时的 p50、p99、p99.9** 搜索参数，不把一个指标的最优配置当作全部指标的最优配置。8、16、32、64 字节各自搜索。这里的 M 是百万条消息；另报有效负载 GB/s，均使用十进制单位。

`payload_bytes` 是业务负载净长度，序号占用其中前 8 字节。本基准统一使用 `Writer<publish_count, false>`，不写长度前缀；批次回调按编译期固定的 8/16/32/64 字节拆包，再逐条调用同一个 Sink。只统计这个固定长度版本，包含第二层复制与独立回调线程。外部负载默认对象保留 `bytes{}` 初始化，普通拷贝构造使用 `bytes(other.bytes)` 复制全部字节；rigtorp 可通过原始 `try_emplace` 请求目标写预取，ReaderWriterQueue 可请求先清零再完整 memcpy。发布配置仅为 ReaderWriterQueue 16/32 B 开启 `readerwriterqueue_copy_after_zero`，8/64 B 保留成员复制，额外清零也计入写入时间；三个开源队列的原文件保持不变。消息的每个 8 字节字都随序号变化，避免常量尾部让测试过度依赖编译器的常量传播。

## 构建与运行

需要 Python 3.9+、CMake 3.20+ 和 C++20 编译器。第三方代码只用于此可选基准，普通库构建不下载、不依赖它们。固定 commit、文件 SHA-256 和许可证记录在 `spsc_dependencies.json`；下载器仅取所需头文件与许可证，不执行第三方构建脚本。

```sh
python3 benchmarks/bootstrap_spsc.py
python3 benchmarks/bootstrap_spsc.py --check-only
cmake -S . -B build-spsc -DCMAKE_BUILD_TYPE=Release -DBRQ_BUILD_SPSC_BENCHMARKS=ON -DBRQ_BUILD_TESTS=OFF -DBRQ_BUILD_EXAMPLES=OFF -DBRQ_BUILD_FUNCTIONAL_TESTS=OFF
cmake --build build-spsc --config Release --target brq_tunable_benchmark --parallel
python3 benchmarks/tune_spsc.py ./build-spsc/brq_tunable_benchmark --profile benchmarks/profiles/spsc_quick.json --output benchmark-results/my-machine
```

Windows Visual Studio 的程序路径为 `build-spsc/Release/brq_tunable_benchmark.exe`，Ninja/MinGW 为 `build-spsc/brq_tunable_benchmark.exe`。多配置构建器必须显式构建 Release。脚本拒绝未优化或启用 sanitizer 的程序。`--allow-debug` 仅供功能检查。

macOS 需要 14.4+ SDK/系统。默认 Apple Clang 构建不需要额外关闭告警或替换 libc++；也可以显式指定自己的 C++20 编译器。

### 正式的普通 write / WriteSession 对照

构建优化、未启用 sanitizer 的基准后执行：

```sh
python3 benchmarks/run_release_benchmarks.py --binary ./build-spsc/brq_tunable_benchmark --output docs/performance_data/my-release --report-dir docs
```

默认运行 8/16/32/64 B、每次 1,000 万条、3 次取算术平均，保留所有试验。8 MiB 回调循环为 0；256 MiB 以普通 write 校准回调区间/发送时间比约 2～3，再给同长度的 Session 和三个开源对照使用相同循环次数。若校准预算内未达到目标，保存 target_met=false 与实际观测，不宣称目标已达到。该比值包含回调分发、等待和调度，不是回调 CPU 时间之和。

本轮另做了 [复制构造敏感性对照](../docs/performance_data/copy_sensitivity_20261011/summary.json)：只改变外部 Payload 的复制构造，保持库、队列、容量、输入、时钟和调度条件相同，12 组各 5 次测量加 24 次完整校验。部分实现的差距显著，不能当成队列算法的优势；具体处理器层面的机制仍未确定。早期四份报告统一使用 `bytes(other.bytes)`。当前发布配置按上文记录调用端的可选优化；不能把不同复制路径的所有差距算成队列算法优势。开关在计时前选择模板，原始源码和同一程序开关对照均保留。可加 `--copy-sensitivity docs/performance_data/copy_sensitivity_20261011/summary.json` 附上该对照。

参数保存在 `profiles/spsc_release.json`。普通 write 开启 current 扫描，初始/最大扫描间隔和最小等待均为 1 ms，每 10 次发布；Session 保留关闭扫描和 UINT32_MAX 发布次数，两者池容量相同，读端/发布配置不同，不能称为同参数对照。每层数据池最多 8/256 MiB，原生 SPSC 保留槽和 rigtorp 首尾 padding 纳入 payload 存储预算；元数据与准备输入另计。此固定参数复测不宣称全局最优；其他平台可以另外运行 tune_spsc.py 寻优并保存自己的参数。

脚本生成四份 HTML 和新的结果目录：原始 JSONL、完整构造参数、逐次命令、源码 tar.gz、源码/依赖/可执行程序 SHA-256、编译命令与环境。既有四份 HTML 先归档。目录不能覆盖；中断后加 `--resume`，只允许源码、二进制和参数完全一致，已完成的试验不会重复。普通与 Session 报告共享本轮实际测得的原生 SPSC 数据，不混用历史结果。

本机默认使用 user_interactive QoS，未硬绑核；正式本机运行要求交流电供电。其他平台默认不改变 QoS，支持时可设置 `--producer-cpu N`；脚本与测试类不会声称无法实现的绑核。离线依赖目录传 `--deps /path/to/deps`；可用 `--timer steady` 强制标准时钟。若系统限制硬件查询，原始环境记录 unavailable，也可通过 `--hardware-info hardware.json` 附独立采集的型号与核心信息，文件及哈希一并保存。Windows 多配置程序位于 `build-spsc/Release/brq_tunable_benchmark.exe`。

只重测普通 write 并保持 Session 报告原样，可加 `--write-mode normal`。为隔离本次扫描/发布配置变化，可加 `--callback-iterations 1 1 2 4` 沿用 256 MiB 的固定循环次数；此时不重新校准，实际时间比可能不在 2～3 倍内。8 MiB 仍为 0。生成时只归档和覆盖选中的报告，开源对照重新测量。

快速校验脚本和报告（这不是正式成绩）：

```sh
python3 benchmarks/run_release_benchmarks.py --binary ./build-spsc/brq_tunable_benchmark --output benchmark-results/smoke --report-dir benchmark-results/smoke/reports --cache 8 --messages 10000 --repetitions 1
```

离线环境可在联网机器下载依赖，再通过 `-DBRQ_SPSC_DEPS=/absolute/path/to/deps` 指向同一目录。每次 CMake 配置均校验文件哈希。

## 在自己的程序中使用测试类

头文件在 `benchmarks/tunable/`，不是库热路径的一部分。链接 CMake 的 `brq_performance_support` 目标即可获得库和三方头文件路径。

```cpp
#include "tunable/performance_test.hpp"
#include <iostream>

int main() {
    namespace perf = TwoBufQueue::performance;
    perf::PerformanceConfig config;
    config.payload_bytes = 8;          // 本轮需求固定
    config.messages = 5'000'000;       // 正式成功 write 次数
    config.latency_batch_size = 1000; // 延时测试每组写入次数
    config.callback_iterations = 0;   // 每条消息回调内的模拟计算循环
    config.source.block_size = 65536;
    config.output.output_block_size = 65536;
    config.publish_count = 4;
    config.mode = perf::MeasureMode::throughput;
    const auto result = perf::PerformanceTest(config).run();
    std::cout << result.m_messages_per_second << " M msg/s\n";
}
```

`PerformanceTest` 构造函数接收整个结构体并校验。`run()` 每次创建新队列与线程；模板分派在正式计时前完成，write 调用不经过虚函数或 `std::function`。回调对每条消息运行相同的 xorshift 计算循环，并使用编译器屏障和最终 checksum 防止消除。`callback_iterations=0` 跳过模拟业务计算，但仍保留框架必需的顺序检查、计数与末条通知。因此这里不是“完全没有回调成本”。搜索脚本还检查不同实现的计算 checksum 一致。

`timeout_seconds` 到期将终止当前测试进程；搜索驱动为每一轮启动独立进程。

整轮预备输入数组独立于队列，8/16/32/64 字节都按同样方式逐条读取和复制。它可能大于 L1 缓存，因此这是连续读取输入的工作负载，不能声称输入常驻 L1。输入的最大字节数见 `prepared_input_bytes`，默认上限 256 MiB；需更多时显式调整上限，或使用分轮。端到端 `wall_seconds` 从首轮输入准备完成后开始，因此包含后续轮次的输入准备及全部排空，但不包含首轮准备；所有准备时间另行完整报告。

吞吐循环在进入每轮时缓存输入地址、循环边界和目标队列。连续发送与限速发送通过模板在循环外选择；提前准备输入的连续吞吐循环直接迭代输入指针，失败重试按值保存队列和消息地址，避免每条消息从栈上重新加载队列地址。连续模式不逐条判断限速设置；只有单次延时采样和失败重试路径读其所需时钟。循环与正常 write/try 接口自身的判断仍计入发送时间。

## 参数

`--defaults` 输出完整默认结构体的扁平 JSON。所有字段都可通过 `--字段 值` 传入 CLI。C++ 中使用嵌套的 `source`、`output`、`output.read_config`；字段映射集中在 `parameters.hpp`。

| 分类 | CLI 字段 | 含义 |
|---|---|---|
| 固定工作负载 | `payload_bytes`, `messages`, `warmup_messages` | 负载长度、正式条数、独立预热条数；预热向上调整到自然发布位置，实际值见 `effective_warmup_messages`；0 时仍发送一组握手消息 |
| 固定输入 | `prepared_input`, `variable_payload`, `prepared_input_limit_bytes` | 默认整轮提前准备完整可变负载；输入缓冲单独限额并另报峰值字节数，不算队列容量。输入方式不得参与调优 |
| 固定工作负载 | `callback_iterations`, `producer_interval_ns` | 每条回调循环次数；生产间隔 0 表示连续输入，非 0 用忙等节拍 |
| 源块 | `source_block_size`, `source_pool_min`, `source_pool_max` | 源块字节数、池最小/最大块数 |
| 源块交接 | `source_queue_capacity` | 至少 2 个交接项 |
| Writer 发布 | `publish_count` | 每 1～1024 条自然发布写指针（2 的幂）；测试不调用主动 flush，换块与 finish 保持库原有行为 |
| Writer 背压 | `writer_backpressure`, `writer_spin_count`, `writer_wait_timeout_ms` | drop/block、自旋次数、块池等待超时；drop 返回失败后驱动仍重试，不丢消息 |
| 第二层 | `output_block_size`, `output_frame_capacity` | 输出块字节数、每块范围描述容量；输出块须不小于源块 |
| 第二层池 | `output_pool_min`, `output_pool_max`, `output_queue_capacity` | 输出池与交接队列容量 |
| 第二层队列 | `cancel_claim_mode` | tail/keep，与库的 QueueTail/Keep 对应 |
| 输出执行 | `output_mode`, `output_reader_mode` | 默认 mpsc/wait_signal；可选 sync/spsc/mpsc/spmc/mpmc、polling/wait_signal |
| 输出消费 | `output_reader_count`, `output_retry_wait_us`, `output_enable_stats` | 基准只允许 1 个回调线程；拒绝重试等待与输出统计开关 |
| 读取等待 | `reader_spin_count`, `rescan_interval_ns`, `rescan_max_interval_ns` | 自旋次数、当前块初始/最大扫描间隔 |
| 收尾等待 | `finish_timeout_ms`, `work_wait_min_ms`, `finish_retry_ms` | 读取器收尾超时、最小等待、重试间隔 |
| SPSC | `spsc_capacity` | 请求消息槽数；0 按源池可容纳记录数计算，另报第三方实际取整容量 |
| SPSC 等待 | `spsc_idle_spins`, `spsc_idle_sleep_ns` | 消费者空队列自旋次数，之后 0=yield、非 0=sleep |
| 调用方重试 | `retry_yield_every` | 失败重试时每 N 次 pause 后 yield；0 始终 pause |
| 分轮 | `round_storage_bytes`, `spsc_buffer_bytes`, `prefault_producer_buffers` | 每轮存储字节阈值、SPSC 数据容量字节、计时前触碰写端页；round=0 为原端到端口径 |
| macOS 调度 | `producer_qos` | unchanged/user_interactive；高 QoS 仅是调度偏好，不能保证核心或禁止迁移 |
| 亲和性 | `producer_cpu`, `consumer_cpu` | -1 不绑定；Linux/Windows 可指定 CPU，macOS 请求绑定会明确失败 |
| 测量 | `mode`, `timer` | throughput/write_latency/single_write_latency/call_batch；auto/steady |
| 延时分组 | `latency_batch_size` | 默认 1000；每组成功写入次数，组总耗时除以此值 |
| 抽样 | `sample_every`, `max_samples`, `random_seed` | 单次模式的平均抽样间隔、每类样本上限、随机种子；分组模式逐组采样 |
| 校准 | `timer_calibration_samples`, `call_batch_size` | 空计时校准次数、批次参考的写入数 |
| 校验与约束 | `verify_payload`, `timeout_seconds`, `buffer_budget_bytes` | 全字节检查、独立测试进程超时、名义数据缓冲预算 |

写线程在所有后台线程创建后才设置 `producer_cpu`，避免后台线程继承写线程的亲和性。`consumer_cpu` 对 TwoBufQueue 指复制线程，对 SPSC 指消费线程；输出回调线程不额外绑核。

默认源池/输出池 min=max，避免正式测试中增长；可显式搜索增长策略。预算约束的是名义数据缓冲：源块池加输出块池，或 SPSC 实际槽数乘负载字节数，不含所有元数据、线程栈和框架样本数组。另报整个进程 peak RSS，不能将两者混同。`source.thread_id=0`、`read_layer1_thread_count=1` 固定，本测试只有一个生产者。

## 测量边界与很短的 write

1. **吞吐**独立运行。`round_storage_bytes=0` 时，从正式生产开始到最后一条回调完成且生产者 finish 完成，包含写入、失败重试和排空。分轮时只累计各轮正式发送区间，轮末排空与最终 finish 在区间外。复制与回调始终在后台并发运行。默认 `prepared_input=true`：整轮输入在发送计时之前准备，输入生成不计入写端吞吐，时间另报 `input_preparation_seconds`。`prepared_input=false` 只用于诊断旧方式，吞吐会包含生成负载的开销，不能与默认口径混合比较。吞吐不逐条读时钟。
2. **write_latency**默认按组计时，`latency_batch_size=1000`。默认模式与吞吐共享整轮提前准备的同一输入数组，从中取每组连续记录，然后开始计时、循环成功写入 1000 条、结束计时，用总耗时除以 1000，作为一个样本。组内循环、失败重试及等待都在区间内；失败次数单独报告，不改变成功写入/消费总数。该模式只接受连续输入。
3. **这里的 p50/p99/p99.9 是各组平均耗时的分位数，不是单次 write 的分位数。** 分组会摊薄计时开销，也会平滑单条写入的尖峰。JSON 使用 `latency_kind=batch_average`、`batch_average_write_ns`，不会写入 `successful_write_ns`。同一轮比较所有实现使用相同组大小，组大小不能参与队列参数调优。
4. 分组模式每个完整组收集一个样本，达到 `max_samples` 后停止收集；后续数据仍正常写入。最后不足一整组的尾部数据也写完，但不进入分位数，条数记在 `latency_tail_writes`。`messages` 至少应容纳一个完整组。`sample_every` 只影响单次模式。
5. **single_write_latency**保留原来的单次调用测量。计时只包一次 `writer.write(payload)` / `try_push(payload)` / `try_enqueue(payload)`；调用内的换块/等待计入，驱动层的重试等待不计入。成功/失败调用分别统计分位数，采样间隔伪随机变化。使用该模式时，低频输入可由 `producer_interval_ns` 指定。
6. 支持 invariant TSC 与 RDTSCP 的 x86 平台使用校准后的 LFENCE/RDTSCP，并剔除检测到的 CPU 迁移样本。**Apple M3 是 ARM，不能执行 x86 的 RDTSC/RDTSCP。** 本机实测 ARM 通用计数器 24 MHz，mach timebase 为 125/3，即约 41.67 ns/tick；直接读相同计数器不能提升刻度。其他平台回退 steady_clock，其 period 只是名义 tick，仍需看空计时校准。
7. 每组 1000 次时，组平均值的量化步长约为 `41.67/1000=0.04167 ns`，这是平均值的步长，不是单条调用获得了这种精度。记录 `timer_quantum_ns` 和缩放后的 `latency_quantum_ns`；不机械扣除计时开销。报告按每项指标保守判断：任一轮的分位数为 0、低于 5 个有效 tick 或两倍缩放后的空计时中位数时，不评选该项最优，仅保留按吞吐选择的代表配置。此阈值不是置信区间。
8. **call_batch**保留旧的批次总体均值参考，不参与分位数或最优排名。它遇到失败先结束本批、在区间外等待，因此不是 write_latency 分组模式的同义词。
9. 分位数使用 nearest-rank；多轮汇总为各轮分位数的算术平均（默认独立运行 3 次）。默认 1000 条/组，正式写入 1 亿条可获得 10 万组样本。每轮少于 10 万有效样本会提示 p99.9 尾部样本偏少；应增加 messages 或减少组大小，而不是把组内条数当作独立延时样本。full profile 使用每轮 1 亿条正式消息，按 8 MiB 输入上限分轮；quick 也使用这个输入上限，smoke 适合流程检查。较小源池仍可能背压，须按具体目标配置池容量。

可直接运行默认分组延时测量：

```sh
./build-spsc/brq_tunable_benchmark --mode write_latency --latency_batch_size 1000 --messages 100000000 --payload_bytes 8 --round_storage_bytes 8388608
```

这里没有测消息从生产到回调的传递延时。最小 write 延时配置可能积压更久、吞吐更低，需要结合吞吐、失败次数和资源使用选择。SPSC 用其原生非分配 try 接口和轮询消费；TwoBufQueue 用自身的事件/自旋策略，包含第二层复制，当前包装器默认使用独立输出回调线程：TwoBufQueue 为一个写线程、一个复制线程、一个回调线程；三个 SPSC 对照为一个写线程和一个消费线程，各实现另有一个不参与消息处理的测试看门狗线程。`output_mode=sync` 可显式选择复制线程同步回调；输出模式会写入完整参数，不能混同为等线程数比较。它们的缓冲组织不同，不能解释成严格等内存的算法对比。每行同时保留 CPU 核数（CPU 秒/墙钟秒）、请求/实际容量、名义缓冲与 RSS。

## 搜索与复现

三个 profile：`spsc_smoke.json` 验证流程；`spsc_quick.json` 有限候选快速搜索；`spsc_full.json` 扩大预算与复测次数。全部默认搜索 8/16/32/64 字节，回调循环为 0。若要测回调负载，可将顶层 `callback_iterations` 改成 `[0, 10, 100]`，每个值是独立工作负载，不混在一起选最优。

```json
{
  "payload_bytes": [8, 16, 32, 64],
  "callback_iterations": [0],
  "base": {"warmup_messages": 50000, "sample_every": 16},
  "search": {
    "strategy": "coordinate", "max_candidates_per_transport": 24,
    "passes": 2, "screen_messages": 1000000, "final_messages": 5000000,
    "screen_repetitions": 2, "final_repetitions": 3, "validation_messages": 10000,
    "batch_reference": true
  },
  "spaces": {
    "two_buf_queue": {"publish_count": [1, 4, 32], "block_bytes": [4096, 65536]},
    "spsc": {"spsc_capacity": [1024, 8192, 65536]}
  }
}
```

`base` 固定其他参数。`spaces` 可以包含上表中的调优字段；工作负载、抽样和测试约束不能放入 spaces，避免不同实现改变需求获得虚假优势。SPSC 仅接受与其有关的容量、等待、重试和 CPU 字段。便捷别名 `cache_block_bytes` 在改变块大小时同时反向调整两个固定池的块数，保持各层数据字节预算不变，并按块数比例调整交接容量；要求预算能整除新块大小，不能与池/块/交接容量字段同时搜索。其他便捷别名 `block_bytes` 同时设置源/输出块大小，`source_pool_blocks` 与 `output_pool_blocks` 同时设置各自 min/max。

`coordinate` 先测试基线和每个维度的变化，再围绕四个指标各自的优胜候选组合参数，受预算和 passes 限制。`grid` 测笛卡尔积；有限预算时取固定随机种子的子集，`max_candidates_per_transport=0` 才是不限制候选数。基线总会纳入比较，即便基线值不在 spaces 中。候选先在独立全负载校验进程通过 FIFO/内容/条数检查，再随机交错运行无时钟吞吐和分组延时。初筛各指标的优胜者用独立正式轮次复测，再从这些复测候选中分别选出可分辨指标的最优；精度不足的指标不排名。任何缺轮、校验失败或超时的候选不得依靠部分成功数据获胜。

**最优是本次搜索空间和预算中的实测最优，不是全局最优。** 坐标搜索可能遗漏参数联合作用；需要时扩大空间/预算或改用穷举。每次调用是独立进程，并保留完整命令，便于避免同进程高水位 RSS 和历史状态干扰。多次正式复测也可能受调度、温度、功耗和后台任务影响。

输出目录必须为空，以保留已有实验：

| 文件 | 内容 |
|---|---|
| `SUMMARY.md` | 按长度、回调负载、实现和优化目标展示指标 |
| `best.json`, `best.csv` | 可排名的最优指标及完整参数；JSON 的 unresolved 保存无法排名的代表配置 |
| `unresolved.csv` | 无法排名的指标，objective_value 为空，不把 0 当作最优耗时 |
| `candidates.csv` | 所有成功初筛/正式候选的指标和参数 |
| `raw.jsonl` | 每一轮原始指标、实际配置、命令、阶段、种子 |
| `failures.jsonl` | 失败命令与错误，不参与排名 |
| `environment.json` | 平台、编译器、程序和源码 SHA-256、第三方固定版本、profile |

复现某个优胜者时，从 best.json 的 `parameters` 取参数，设定与正式阶段相同的 messages/mode；或直接执行 raw.jsonl 中对应正式轮次的 command。发布成绩时补充 CPU 型号、功耗策略和后台负载，测试时关闭编译和其他压测任务。不应根据 smoke 的短样本宣布最快 SPSC。

## 功能检查

```sh
cmake -S . -B build-spsc -DBRQ_BUILD_TESTS=ON -DBRQ_BUILD_FUNCTIONAL_TESTS=OFF
cmake --build build-spsc --config Release --target test_performance_harness brq_tunable_benchmark --parallel
ctest --test-dir build-spsc -C Release -R '^performance_' --output-on-failure
python3 benchmarks/tune_spsc.py ./build-spsc/brq_tunable_benchmark --profile benchmarks/profiles/spsc_smoke.json --output benchmark-results/smoke
```

契约检查覆盖不同负载长度、四个实现、四种测量模式、逐条回调循环 checksum、不同 Writer 发布策略和尾部收尾、参数验证、抽样上限，以及独立指标选优与工作负载冻结。不设机器相关的性能通过阈值。

## 大缓冲分轮测写线程

`profiles/spsc_burst.json` 提供异步分轮方案：源池与输出池各为 256 MiB，输出使用 SPSC 队列，复制线程和回调线程各自独立。源池也足以暂存整轮，减少复制跟不上时对写线程的背压；源交接/输出队列容量各为 512。第三方请求 256 MiB 原生消息槽容量，仍记录各库实际取整容量与 RSS。这不是严格等内存测试。

每轮最多 `floor(214748364/payload_bytes)` 条，即约 204.8 MiB 的固定长度业务负载，四种库的写入次数相同。这里没有长度头。frame 数量也可能使输出块提前提交，不能把名义容量解释成精确的可积压业务字节数。

测试不主动调用 flush，包括预热结束、逐条发送和轮末。每轮上限仍按 `round_storage_bytes / payload_bytes` 向下取整，再按源块内的自然发布位置略微向下调整。不能只按全局消息数对齐，因为 Writer 在换块时重新开始发布计数。取能容纳的最大候选发布组（从 1024 到 1 的 2 次幂），相同源配置下各库和各 `publish_count` 候选使用相同的预热条数与分轮边界；预热最多增加 `2*(publication_alignment-1)` 条，非末轮最多减少同样数量。源块和轮容量必须能容纳配置的发布组，否则明确拒绝该配置。实际边界元数据见 `effective_warmup_messages`、`publication_alignment`、`source_record_bytes`、`round_message_limit` 和 `round_message_minimum`。

正式计时只累计每轮发送区间：最后一条成功写入后停止计时，等待本轮全部逐条回调完成，再开始下一轮。Writer 跨轮复用；最后一轮停止计时后正常 finish，发布最终尾部，再等待回调完成。统计 `producer_seconds`、`wall_seconds`、`drain_seconds`、`completed_rounds`、失败重试次数。排空与最终 finish 不计入吞吐和延时组。分组不跨轮，末尾不足一组仍发送但不纳入分位数，计入 `latency_tail_writes`。

本口径报告写线程的分轮发送能力，不代表从发送到回调结束的持续端到端吞吐。复制/回调线程与写线程仍并发运行，可能竞争缓存、带宽、调度资源；高 QoS 不能消除这些影响。CPU 核数仍按整个墙钟时间计算。预触碰仅覆盖写端源池/第三方消息存储，不预触碰私有的第二层池，结果保留这项限制。

初筛与正式复测各独立执行 3 次，算术平均吞吐；每次先计算 p50/p99/p99.9，再对对应分位数做算术平均，不合并各次样本。各长度初筛至少两整轮，正式至少两轮且至少 1 亿条；因此每个正式延时进程至少约 10 万组样本。当前分轮 profile 搜索 publish_count=1/4/16/32，第三方使用固定的大容量配置；它只寻找声明范围内的最优值。

```sh
python3 benchmarks/tune_spsc.py ./build-spsc/brq_tunable_benchmark --profile benchmarks/profiles/spsc_burst.json --output benchmark-results/burst
```

Linux/Windows 可在 profile 的 base 中设置 `producer_cpu`，macOS 则设置 `producer_qos: "user_interactive"` 并保留 `producer_cpu: -1`。写线程的亲和性/QoS 在后台线程启动后设置；原设置在 run 结束时恢复；原 QoS 为 UNSPECIFIED 时设回 DEFAULT，避免留下高 QoS（macOS 不允许 setter 传入 UNSPECIFIED）。结果分别记录 `producer_hard_affinity`、`producer_qos_applied`、`producer_qos_actual`；本机高 QoS 数据必须注明“未硬绑核”。不支持的绑核/QoS 请求明确报错，不能静默回退。分轮模式拒绝 Sync 输出模式。

## 校准较慢回调并对比

`profiles/spsc_callback.json` 使用 256 MiB 池/队列预算，与分轮 profile 相同，启用 `measure_callback_time=true`。回调仍对每条消息运行同一个 xorshift 循环；不同库共用 Sink，且按长度使用相同 callback_iterations，绝不为慢的开源写入接口单独减少循环。

脚本先以 TwoBufQueue 的 publish_count=16 为基准，分别校准 8/16/32/64 字节负载。每个候选循环次数独立运行 3 次，目标回调处理时间约为写线程时间的 2～3 倍；从正整数 1 次开始、指数扩展后折半搜索，结果固定后才搜索各库的吞吐/延时参数。callback_iterations 字段不能与 callback_calibration 同时指定，后者决定本轮共同工作负载。校准观测和目标是否满足保存在 callback-calibration.json；若整数步长或预算无法命中目标，保留最近的正整数并明确标记 target_met=false。

回调计时在每轮首条回调入口和末条回调业务执行结束处各读一次 steady_clock；不逐条加时间戳。callback_processing_seconds 是这些处理区间之和，包含轮内消息分发、等待与调度时间，排除预热和轮间排空后的等待；它不是纯回调函数 CPU 耗时之和。callback_to_producer_ratio 为该值 / producer_seconds，timed_callback_messages 必须等于正式成功写入次数。回调比写线程慢时，此口径适合本次粗略负载校准。不同队列写入速度不同，因此相同循环次数下，其余实现的比例可以不同；正式最优参数的比例也会与固定参考配置略有差异。

```sh
python3 benchmarks/tune_spsc.py ./build-spsc/brq_tunable_benchmark --profile benchmarks/profiles/spsc_callback.json --output benchmark-results/callback
```

callback_calibration 可配置 target_ratio/min_ratio/max_ratio（默认 2.5/2/3）、repetitions（3）、messages（100 万）、min_rounds（2）、max_iterations（1024）、max_candidates（10）、reference_publish_count（16）。本机在 base 设置 producer_qos=user_interactive，仍注明未硬绑核。每个正式进程至少写入 100,100,000 条；组大小 1000，各指标独立选择，3 次算术平均。原生队列的保留槽、取整与元数据不同，名义数据容量统一为 256 MiB，但不能承诺 RSS 完全相同；TwoBufQueue 有两层数据池，合计 512 MiB。


## 背压统计与回调时间比

所有实现报告 `failed_write_calls`（失败尝试数）、`backpressured_messages`（至少有一次非阻塞写失败的消息数）、`producer_retry_seconds`（第一次失败返回后至该消息重试成功的累计时间）及 `producer_retry_max_seconds`（最长一次重试区间）。重试时间包含 pause/yield、重试调用与测量开销；成功写入不读重试时钟。失败消息持续重试直至成功，计数不代表最终丢失业务消息。Block 模式的 Writer 内部等待另看库内统计，不能仅凭 `failed_write_calls=0` 判断没有内部阻塞。

TwoBufQueue 的 `library_backpressure` 同时记录 Writer 的 `writer_pressure_events`、`writer_rejected_attempts`、`writer_blocked_events`、`writer_blocked_ticks`，以及复制线程的 `copy_memory_full_events` 和去重后的 `copy_backpressured_frames`。这些是预热结束后的增量，覆盖正式发送、轮末排空与收尾；`writer_blocked_ticks` 为库原始计数，x86 是 TSC tick，其他平台为 steady_clock tick，不直接当成纳秒。复制线程次数按块/帧统计，不与逐条 push 失败次数直接比较。

三个开源队列通过原生非分配 try 接口的 false 返回统计生产端受压情况；没有接入 TwoBufQueue 的块/复制统计，因此 `library_backpressure=null`，不是零。三次汇总取各次指标算术平均。每个候选的吞吐模式和延时模式分开，不把延时模式的背压数混入吞吐统计。

`callback_to_producer_ratio` 是逐次“回调处理区间 / 写入发送区间”的三次算术平均，不是两个平均时间相除。回调处理区间从每轮首条回调入口到末条业务结束，包含消息分发、轮内等待和调度，不是纯回调函数 CPU 时间之和。大于 1 不自动等于写端发生背压，须结合失败/内部阻塞计数；零背压也不能排除缓存或带宽竞争。


## 保存离线 HTML 对比报告

需要先修正测试口径、再选参数时，使用下面的搜索及导出入口。8 MiB 场景固定空业务循环；256 MiB 场景按长度校准正整数循环次数。两个场景均为一千万条正式消息、三次独立复测，各指标分别选择配置。输入提前准备、全负载内容变化、缓存数据字节预算及消息长度都被冻结；`cache_block_bytes` 改块大小时保持两层池各自的字节预算不变。

```sh
python3 benchmarks/tune_spsc.py ./build-spsc/brq_tunable_benchmark --profile benchmarks/profiles/spsc_tuned_8m_zero.json --output benchmark-results/tuned-8m
python3 benchmarks/export_spsc_report.py benchmark-results/tuned-8m --cache-mib 8 --callback-policy zero --output docs/performance_data/tuned_8m_zero --html docs/performance_8m_zero.html
python3 benchmarks/tune_spsc.py ./build-spsc/brq_tunable_benchmark --profile benchmarks/profiles/spsc_tuned_256m_calibrated.json --output benchmark-results/tuned-256m
python3 benchmarks/export_spsc_report.py benchmark-results/tuned-256m --cache-mib 256 --callback-policy calibrated --output docs/performance_data/tuned_256m_callback --html docs/performance_256m_callback.html
```

256 MiB profile 的 `base_by_payload` 给每种长度设置校准参考参数，同时作为搜索起点。该起点来自本机 8 MiB 的搜索，其他平台仍会搜索并复测，不能视为通用最优。`cache_block_bytes` 参考值仍保持新场景各层 256 MiB 的预算。`callback_calibration.min_rounds=0` 保持指定的一千万条校准消息，不因容量更大而自动增加条数；同一长度的四个实现和四个优化目标使用完全相同的回调循环次数。

这两个 profile 使用两轮坐标搜索，每种长度 / 实现最多测试 64 个候选。TwoBufQueue 搜索 64 KiB / 256 KiB / 1 MiB 块、1～1024（2 次幂）发布批次、1 / 64 / 1024 个 frame、自旋 0 / 1000 / 10000 次及 1 / 5 / 30 ms 初始扫描间隔。第三方搜索消费者自旋和空闲睡眠设置，容量始终不变。坐标搜索受预算限制，可能遗漏参数之间的联合作用。可用结构体及其他 profile 扩展读取、池、统计、背压等参数，但不能把输入准备方式、消息长度或回调次数作为获得更好成绩的调优项。

导出器直接使用搜索中独立正式复测的原始数据，不再选择另一轮较好成绩。它校验各指标的完整三次记录、缓存预算、正式条数、完整负载校验及回调循环一致性。表中吞吐、p50、p99、p999 可以来自不同参数，完整参数按指标展开；这些分位数仍是 1000 次写入组均值的分位数。完整搜索日志另存 `search-raw.jsonl`，选中的正式及校验日志存 `raw.jsonl`。最优仅指声明范围和预算内的观测结果，不代表全局最优或稳定的尾延时承诺。

`report_spsc.py` 固定参数运行四种实现，只保留 TwoBufQueue 不带长度前缀的版本；这与参数搜索脚本是两个独立入口。报告包括吞吐、1000 次写入组均值的 p50/p99/p999、回调时间比、背压、三次原始值和完整构造参数。各消息长度有固定背景色，TwoBufQueue 整行加粗并加边框；页面可离线筛选、导出 JSON 和打印。

```sh
python3 benchmarks/report_spsc.py ./build-spsc/brq_tunable_benchmark --profile benchmarks/profiles/spsc_report_256m_calibrated.json --output docs/performance_data/256m_callback_my_run --html docs/performance_256m_callback_my_run.html
python3 benchmarks/report_spsc.py ./build-spsc/brq_tunable_benchmark --profile benchmarks/profiles/spsc_report_8m_zero.json --output docs/performance_data/8m_zero_my_run --html docs/performance_8m_zero_my_run.html
```

两个报告 profile 都固定 8/16/32/64 字节、一千万条正式写入、各指标三次平均、`is_scan_current=false` 和 `publish_count=4294967295`（`UINT32_MAX`）。Writer 写入时只推进本地游标，不逐条计数或发布；仅读取完成块。256 MiB 场景按长度重新校准，使 TwoBufQueue 的正式吞吐运行中整轮回调时间大约为写线程的 2～3 倍，再让所有开源对照使用相同循环次数；8 MiB 场景循环为 0。每个源池、输出池分别为声明的缓存大小，第三方请求同样的业务字节容量；源块/输出块保持 64 KiB。每轮上限为缓存 80%；完成块模式在发送计时结束后交接当前块，再等待排空。回调时间比含消息分发与轮内等待，不能解释成纯回调函数耗时之和。

本机 profile 使用高 QoS、未硬绑核。其他平台应调整 `base.producer_qos` 与 `base.producer_cpu`，可以继续使用同一测试程序。输出目录必须不存在，避免覆盖已有测量。HTML 同目录保留 `performance_data` 后，页面中的原始日志链接也可离线使用。每次一千万条、组大小 1000 时约有一万个延时样本；少于框架建议的十万个样本，报告会明确提示 p999 的限制。

### 完成块模式与时间比

CLI 使用 `--is_scan_current false --publish_count 4294967295`。参数会进入结果 JSON；此前的完成块报告 profile 和对应调优 profile 使用此策略。当前发布 profile 中仅 Session 保留它，普通 write 改为扫描 current、每 10 次发布。

关闭扫描或使用最大发布次数时，预热写完后以及每轮发送计时结束后，TwoBufQueue 调用 `Writer::finish_block()` 交接当前块，再等全部回调完成。`finish_block()` 不增加消息，不结束 Writer；下一次正常写入申请新块。最终结束仍使用 `finish()`。额外的轮末交接和排空时间不计入发送吞吐或写入组均值，后续写入中的正常块申请及换块仍计时。原生 SPSC 每条消息已发布，轮末无需额外交接。所有实现采用相同的预热条数与轮消息上限。

表中“回调区间 / 写入墙钟时间比”按每次运行计算：各轮从首条回调开始到末条回调完成的墙钟耗时之和，除以各轮写线程发送耗时之和，再取三次运行的算术平均。回调区间包含消息分发、业务循环、等待数据与线程调度，不能当作逐条回调函数 CPU 耗时之和。比如写线程累计 10 ms、回调区间累计 25 ms，比例就是 2.5 倍；业务循环为 0 时，比例仍可能大于 1。比例大于 1 不等于写线程受背压，背压须看失败尝试和库内压力统计。

## rigtorp 目标写预取

`rigtorp_write_prefetch` 为调用端的可调布尔参数，默认 true。可用 `--rigtorp_write_prefetch false` 复现关闭提示的路径，也可在 `spaces.rigtorp` 中遍历 `[false, true]`。开关在计时前选择模板，写循环不增加动态判断。只使用 rigtorp 原始 `try_emplace` API，数据仍进行一次完整成员复制，其他队列的调用保持现状。提示不是额外写零，不保证所有平台提速。[同一程序对照与汇编](../docs/RIGTORP_PERFORMANCE.html)。

## writer 块大小遍历

`test_per_writer_block_size` 使用同一个 `PerformanceTest` 测量循环，只实例化 32 B、普通 `write` 的路径；支持发布次数 1 和 UINT32_MAX，默认每条 `release` 发布并扫描当前块。入口默认回调循环 0、读取间隔 1 ms、每项 1 千万条；Python 驱动按 1 KiB、2 KiB、4 KiB……1 GiB 遍历，每项独立进程做完整数据校验，再分别跑 3 次吞吐和分组延时。输入准备、分轮排空、回调时间比、背压统计和现有测试一致，轮容量按 reader 实际总容量的 80% 计算。

```sh
cmake -S . -B build-block-size -DCMAKE_BUILD_TYPE=Release \
  -DBRQ_BUILD_SPSC_BENCHMARKS=ON -DBRQ_ENABLE_POOL_DEPTH_STATS=ON
cmake --build build-block-size --target test_per_writer_block_size
build-block-size/test_per_writer_block_size --self-test
python3 benchmarks/test_per_writer_block_size.py \
  --binary build-block-size/test_per_writer_block_size \
  --output docs/performance_data/writer_block_size_run \
  --report docs/test_per_writer_block_size.html \
  --reader-block-multiplier 10
```

reader 单块按 writer 的倍率倍数计算，默认倍率 10；可用 `--reader-block-multiplier` 调整。writer 池上限默认 2 GiB，reader 总量上限默认固定 2 GiB。reader 单块取 `min(writer 块 × 倍率, reader 上限)`，池最大块数向下取整，因此实际总容量不超过上限。比如 writer 1 GiB 对应 reader 单块 2 GiB、输出池最多一块。只有 C++ 显式 `--reader_cache_fixed false`（Python `--reader-cache-fixed false`）才使用旧的按倍率扩容模式。容量按数据块计算，队列、frame、输入和其他元数据另计。各池初始两块（不超过最大块数），以后按需增长；源侧仅预触碰初始块，reader 不预触碰整个大块。虚拟分配容量与实际 RSS 分开报告。

新增 `pool_depth` 在每次成功取块后、已有池锁内采样，没有额外加锁或逐条写入统计。`max_in_use` 是最大借出未归还块数，包含正式测量开始时的快照；`average_in_use` 按成功取块次数加权，未采样时为 null。另记录分配/空闲块数、采样次数、各项总和与对应字节容量。预分配、预触碰和预热不进入这些统计；正式写入、轮间排空和最终停止均包含。汇总表使用吞吐测试深度，延时测试深度另存 JSON；三次的最大深度取最大值，平均深度按全部取块样本加权。

`BRQ_ENABLE_POOL_DEPTH_STATS` 默认 OFF，正常库构建完全不生成统计字段与取块分支。为避免 ABI/ODR 不一致，开启时由 `TwoBufQueue` 的 PUBLIC 编译定义传递到所有使用方。统计开启后换块的计数成本包含在成绩内。逐条写入和发布代码未修改。

驱动参数包括 `--messages`、`--callback-iterations`、`--initial-pool-blocks`、两层缓存上限、块大小范围、倍率、重复次数、轮容量、延时组大小及写线程绑核/QoS。macOS 默认高 QoS、未硬绑核；Linux/Windows 默认保留调度策略，可设置 `--producer-cpu`。

块大小遍历也支持 `--is-scan-current false`：只读完成块，未显式指定发布次数时会同时选择 `publish_count=UINT32_MAX`，平时只更新本地游标，交接块时发布，其他默认值相同。每轮计时结束后按现有框架调用 `finish_block()` 交接尾块并排空；交接与排空不计入写入时间，下一轮取块仍计时。传入独立的 `--output` 和 `--report` 路径即可保存另一份报告。C++ 单项入口对应 `--is_scan_current false`，也默认选择 UINT32_MAX。需要单独诊断两个开关时，可显式传入 `--publish-count 1`（C++ 为 `--publish_count 1`）。

## 单线程 Writer 块大小对照

`test_per_write_block_size_only_writer` 去掉读线程、第二层复制和业务回调，只构造真实 `Writer`、`WriterPool` 及 Writer 必需的交接队列。现有换块通知在写线程中立即领取完成记录，登记已消费字节，并通过原有生命周期回调归还旧块；不读取当前块，不在逐条发送循环中增加换块检查。自然换块、发布、取块、归还和深度统计均计入写入时间，库实现保持原样。

默认仍为 32 B、普通无长度前缀 `write`、每项 1 千万条、预热 5 万条、池上限 2 GiB、初始两块、高 QoS（macOS 未硬绑核）、每组 1,000 次写入测延时。提前准备每轮完整输入，所有字节随序号变化；初始块预触碰和数据准备不计时。两种发布策略使用完全相同的轮边界（默认按同配置 reader 实际总容量的 80% 计算；单线程测试只引用容量，不创建 reader 池）：预热后和轮末都调用 `finish_block()` 交接尾块并归还，尾部交接不计时，下一轮取块仍计时。因此这里只有发布策略及其对应 Writer 编译路径不同，没有异步排空或等待线程。

```sh
cmake --build build-block-size --target test_per_write_block_size_only_writer
build-block-size/test_per_write_block_size_only_writer --self-test
python3 -B benchmarks/test_per_write_block_size_only_writer.py \
  --binary build-block-size/test_per_write_block_size_only_writer \
  --output docs/performance_data/write_block_size_only_writer_run \
  --report-prefix docs/test_per_write_block_size_only_writer
```

构建配置仍需上节的 `BRQ_BUILD_SPSC_BENCHMARKS=ON` 和 `BRQ_ENABLE_POOL_DEPTH_STATS=ON`。驱动自动同时遍历发布次数 1 和 UINT32_MAX，1 KiB 至 1 GiB 共 21 个块大小。每种策略、每个块大小先在独立进程完整校验消息字节和 FIFO，再各运行 3 次吞吐、3 次分组延时；正式计时不读取消息负载，避免校验给缓存预热。没有创建 watchdog 线程：驱动用进程超时保护，C++ 入口另在轮边界检查超时。

输出两份报告：`*_publish1.html` 与 `*_publish_max.html`，以及 `raw.jsonl`、`summary.json`、`summary.csv`、环境/源码/二进制 SHA-256 和每次运行输出。报告有吞吐、GB/s、p50/p99/p999、三次吞吐范围、RSS、实际取块/自然换块/完成块次数、writer 池最大和平均占用/分配深度、背压次数。没有 reader 池或业务回调，故不列这些指标。延时表示每组写入平均耗时的分位数，不是端到端延时。

池占用采样点仍是新块成功借出之后、旧块归还之前：自然换块时可同时占用两块，归还后仅当前块仍被借出；轮首取块的样本为一块。因此采样均值可接近 2，最大分配通常为 2。驱动检查两种策略的消息数、取块次数、自然换块次数和完成块次数一致，全部资源归还且背压为零。

驱动参数可调整块大小范围、writer 缓存上限、初始块数、消息/预热数量、轮容量、重复次数、延时组大小、CPU/QoS 和超时。单项入口例如 `--source_block_size 2048 --publish_count 1 --mode throughput`；池须容纳至少两块，保证 Writer 先取得新块、再交接归还旧块。这里测得的是单线程写入与即时回收的成本，不能直接作为完整 TwoBufQueue 流水线吞吐。


块大小专项测试的每轮上限按 **reader 实际总容量的 80%** 计算，目的是限制积压、避免写端背压。
完整流水线的默认固定模式中，`reader_block_size = min(writer_block_size * reader_block_multiplier, reader_cache_bytes)`，reader 实际容量为 `floor(reader_cache_bytes / reader_block_size) * reader_block_size`。
包括借出和空闲块，不是当前块大小，也不是 writer 池容量。C++/Python 完整流水线入口使用同一公式。单线程旧对照仍按倍率扩容的 reader 容量引用计算，没有 reader 池；须核对配置后再比较两类结果。
`round_storage_bytes=0`（Python `--round-storage-bytes 0`）表示自动计算；显式设置必须不超过该上限。
默认 reader 固定上限 2 GiB、1 千万条 32 B 时，全部约 305 MiB 数据在一轮内连续发送。
复制和回调仍在写入期间并发运行，发送结束后才等待排空；检查正式运行背压计数，不能仅从容量推断没有背压。
独立预生成输入的默认上限为 2 GiB，不增加队列容量，实际只准备当前轮消息。
此前专项报告使用固定 6.4 MiB 轮容量，大于该轮容量的 writer 块在计时期间没有自然换块，不能与新分轮配置混作同一条件。

固定 reader 总量的读回对照：`docs/test_per_write_block_size_readback_fixed_reader_3runs.html`。reader 上限固定 2 GiB，单块按上述倍率并截断；普通 write 的实验版本在完整复制后读回目标首字节，分别测每条发布和 UINT32_MAX。读回只在报告保存的实验源副本中通过 `BRQ_DIAGNOSTIC_READBACK` 编译宏启用。主库热路径未默认启用此实验。


## 固定第二层容量的三种 Writer 模式寻优

`benchmarks/tune_three_writer_modes.py` 使用上述同一个 `PerformanceTest` 和 `ParameterSearch.evaluate`，分别测试 Session、普通 write 每条发布、普通 write UINT32_MAX。默认 8/16/32/64 B、回调循环 0、每项 1,000 万条、三次吞吐与三次分组延时取算术平均；每个候选另做一次全部字节和 FIFO 校验。当前普通异步 Writer 包含复制后的 volatile 首字节读回，Session 不包含；报告依据二进制构建信息标记这一区别。当前默认 QoS 为 macOS 的 user_interactive，未硬绑核。

```sh
python3 benchmarks/tune_three_writer_modes.py ./build-spsc/brq_tunable_benchmark --output benchmark-results/three-writer-modes
```

先完整遍历 Writer 单块 1 KiB～1 GiB，2 倍递增。初始 Writer 总容量为 `max(8 MiB, 2×Writer 单块)`；随后搜索两块与至少 64 MiB 的容量选项。Reader 总容量始终为 2 GiB，Reader 单块从整除这个容量的值中选择，以保证实际数据池总容量不变；初始为 `min(2 GiB, max(1 MiB, 16×Writer 单块))`，局部寻优另测 `max(64 KiB, Writer 单块)` 和 `min(2 GiB, max(64 KiB, 256×Writer 单块))`。还搜索每块帧容量 1/64/1024、读端自旋 0/1000/10000、扫描间隔 1/5/30 ms，并尝试各指标候选附近的组合。这是分阶段候选搜索，不是所有参数的全笛卡尔积或全局最优证明。第一层容量是被搜索的参数，不能将它与固定的第二层容量混为一谈。

所有候选的 `round_storage_bytes` 固定为第二层容量的 80% 向下取整，`fixed_round_boundaries=true`。每轮达到上限后精确停写，不再随块大小向下调整到自然发布边界；不足上限的最后一轮正常发送。预热仍固定 50,000 条；预热后及非末轮结束时，TwoBufQueue 在计时外 `finish_block()` 交接尾块并排空回调，原生 SPSC 在同一边界排空。扫描每条发布模式仍在正式写入中逐条发布并扫描 current。该开关只影响测试轮边界，不改变 Writer 的写入实现；默认 false 保留历史脚本口径。比较原生 SPSC 时应同样开启此开关，使用相同条数、输入、回调、时钟和轮上限。

每个指标分别选候选，再用独立的三次吞吐和三次延时复测确认。主表要求这些正式运行的写入失败次数和 Writer 压力事件都为 0；另保留不限背压的速度排序以及全部原始观测。第二层复制内存满计数单独列出，不视为 Writer 写入失败。生成 `best.json`、`best.csv`、`candidates.csv`、`raw.jsonl`、`failures.jsonl`、`audit.json`、`environment.json`、各阶段的完整配置以及 `report.html`。HTML 按 8/16/32/64 B 统一颜色，支持长度/模式筛选。参数列表只显示消息长度、Writer 单块、Reader 单块、吞吐、p50、p99、p999 七列，各模式分别按消息长度、Writer 单块、Reader 单块升序排列。每个完整参数组合一行，独立复测优先于初筛；相同块尺寸下其他参数不同的行不合并，悬停查看完整参数。延时是每组 1,000 次写入平均耗时的分位数，不能当作单次 write 或端到端延时。三次均保留，不筛掉低结果；p99.9 的样本限制和内部背压计数会记录。

此默认场景的名义两层数据预算最多 4 GiB，准备输入另计。可用 `--lengths 32` 仅测 32 B；`--reader-bytes` 接受不小于 1 GiB 的 2 次幂，运行中固定；`--messages` 与 `--repetitions` 可调整，公开成绩至少重复三次。实际消息量和完成轮数保存在原始结果。


普通异步 `Writer::write`／`write_mem` 现增加复制后的首字节读回：

```cpp
(void)*reinterpret_cast<volatile std::byte*>(begin);
```

它位于 `commit_write` 之前，不替代 `release` 发布；`WriteSession` 与同步回调路径不加读回。有长度前缀时 `begin` 指向已写好的前缀首字节，无前缀时指向负载首字节。此操作的收益依赖平台与负载，须测量，不能预设所有长度都更快。构建信息通过 `ordinary_writer_readback`、`session_readback` 记录这两个路径。

只重跑逐条发布与 MAX 两种普通写入策略、保持 Session 历史数据，可执行：

```sh
python3 benchmarks/tune_three_writer_modes.py build/brq_tunable_benchmark \
  --output results/ordinary_readback \
  --modes publish1 publish_max \
  --seed-results docs/performance_data/three_writer_modes_optimal_20261011/best.json
```

`--modes` 对所有遍历、细化和独立确认阶段均生效；`--seed-results` 将所选模式的历史最优参数纳入测试，首先校验固定负载与环境字段。默认仍遍历三种模式。使用独立输出目录，保留无读回报告。
