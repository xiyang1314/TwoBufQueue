# 连续发送一批消息，对业务性能产生什么影响

当前[正式报告](../docs/performance_burst_from_empty.zh-CN.md)比较几种队列在突发连续发送后，对生产者热路径业务的影响。此前预填充大积压的实验与报告保留在后面的历史章节。

## 一、一次运行得到当前报告

从项目根目录执行：

```sh
python3 benchmarks/run_burst_empty.py --profile docs/performance_data/burst_from_empty_20261013/selected_config.json
```

程序复用已记录的初始化参数，自动准备固定版本依赖并构建 Release，不重新校准回调或挑选业务工作集。默认报告为 `docs/performance_burst_from_empty.html`，新样本写入带时间戳的新数据目录。

流程是 **正常执行业务函数 → 循环发送一批消息 → 立刻执行业务函数**。每批开始前等待队列排空，因此起始积压必须为 0；消费者与复制线程持续运行，不主动暂停。测量后的排空在计时外执行。本流程使用 `prefill=false` 和 `drain_before_burst=true`，没有 80% 初始积压准备。

固定 32 B 消息、48 KiB 业务工作集、64 次回调循环、16 KiB 第一层总池和 256 MiB 第二层／开源槽位容量，发送量为 2、4、8、16 MiB。每种发送量每轮 2,500 个窗口，每个窗口立即测量 4 组业务、每组 16 次，共 10,000 组样本；三轮独立运行。

报告显示发送后业务组均值的 p50，再对三轮取平均。**比值 = 发送后业务耗时 ÷ 正常业务耗时参考值**；本机正常参考值显示为约 13 ns/次，所以本次报告的分母统一为 13。正常参考值来自同批无队列对照，精确数值与 p99/p999 保留在完整结果中。报告中的基本参数、测试环境和复现明细默认折叠。

全部正式消息逐字节校验完整 32 B 内容、FIFO 和条数，检查所有窗口零起始积压及业务校验和一致。分配、缓冲页预触碰、业务预热和排空不计入业务耗时。消息生成、写入及失败重试属于发送阶段，报告表格测量发送后的业务。

`--executable` 可复用已构建程序；`--deps` 指定固定依赖目录，`--compiler` 显式指定编译器。使用同一 `--output` 并加 `--resume` 可继续中断的运行，程序会核对源码、可执行文件与输入配置哈希。硬件与调度可能使数值波动，复现的是完整流程和参数。

仅重新生成报告、无需重跑测试：

```sh
python3 benchmarks/run_burst_empty.py --render-only \
  --output docs/performance_data/burst_from_empty_20261013 \
  --report docs/performance_burst_from_empty.html
```

中英文 README 与报告首页的概览图由 `python3 tools/plot_burst_release.py` 从同一结果生成。图中四种发送量、全部队列及三轮均值都保留，虚线为正常业务参考值。

单独关闭 `prefill` 只会取消准备消息，后续连续窗口仍可能积压；每批从零开始还需要 `drain_before_burst=true`。历史实验的约 200 MiB 初始积压与 80% 规则不适用于当前报告。

## 二、历史测试：预填充大积压的一次运行

从项目根目录执行：

```sh
python3 benchmarks/run_burst_showcase.py
```

此命令自动校验并下载固定版本的依赖、构建 Release 程序，然后一次完成回调校准、24/48/96 KiB 工作集选择、完整正式采样和 HTML 报告。默认报告为 docs/performance_burst_cache.html，原始数据存入带时间戳的新目录。

统一回调遍历 16/32/64/128/256 次循环，选择所有队列在最大 16 MiB 发送量下均保持至少 75% 积压的最小值。回调持续运行，不人为暂停。第一层总池固定 16 KiB，第二层及原生槽位存储均为 256 MiB，发送量为 1、2、4、8、16 MiB。

工作集筛选综合三次独立运行及各发送量；接近最高成绩的候选中优先选择占优条件更多、波动更小的工作集。全部队列采用同一工作集，不给不同队列设置不同业务负载。

**正式测试直接采满每项每轮至少 10,000 个后业务组样本，三轮独立运行，无需另跑采样脚本。** 每次发送后立即测量 64 次业务，每 16 次计时一组并除以 16；重复 2,500 个发送窗口得到 10,000 个样本。每轮按 nearest-rank 计算 p50/p99/p999，再对三轮分位数取算术平均。不会把少量均值推算成尾部分位数，也不扩展到业务恢复后的长时间运行。

前耗时是发送前 16 次业务的平均耗时；后分位数是业务组均值的分位数。所有队列采用同一计时方式，时钟开销不扣除；有限样本和系统调度仍会使尾部波动。原始样本、每轮数值、代码哈希及消息内容/FIFO/条数校验结果全部保留。

慢回调需要处理重复发送的全部消息，耗时取决于机器。控制路径显示进度，计时区不打印、不分配样本存储。样本不足、容量不符、消息校验失败或积压未达目标时拒绝生成正式结果。

selected_config.json 保存完整业务参数、各队列初始化参数，以及编译器版本和优化选项。固定参数重现：

```sh
python3 benchmarks/run_burst_showcase.py --replay docs/performance_data/本次目录/selected_config.json
```

直接采用记录的参数、编译器和优化选项执行完整正式测试，不重新挑选场景。记录的编译器不存在时会提示安装，或通过 --compiler 明确指定替代编译器。硬件、温度和调度会造成计时波动，复现的是方法和参数，不保证每个读数完全一致。

中途停止后使用原输出目录及相同参数，加 --resume。程序检查调用参数和可执行文件哈希，复用已完整保存的进程结果，未完成进程重新运行。--render-only 只从完整结果生成报告。

可调 --business-kib、--bursts-mib、--runs、--selection-repeats、--samples-per-run、--callback-candidates；正式模式至少 10,000 组/轮、三轮。--callback-iterations 可固定统一回调；--executable 可复用已构建程序，否则自动构建；--deps 指定固定依赖目录。

## 三、原独立诊断方式（少量样本，不替代完整分位数测试）

`test_burst_cache` 是独立的32 B突发测试。所有队列的回调始终运行同一段计算，没有人为暂停或睡眠；TwoBufQueue仍使用独立复制线程和回调线程。

测试针对积压较大时的写侧小数据池：写线程先把业务表变热（默认48 KiB，可配置），连续发送一批消息，再立即执行业务。业务表采用打乱顺序的依赖链，每次访问并更新8个节点，所有实现使用同一个不可内联的业务函数。

## 四、历史诊断的参数与流程

- 第一层**总数据池**默认遍历2、4、8、16、32、64、128 KiB，固定4块，所以最小单块512 B。第二层总容量始终256 MiB，Reader单块与Writer单块相同。
- 每次突发默认遍历16 KiB到128 MiB的14档2次幂；全部固定32 B消息，不带长度前缀，所有32 B随序号变化。
- 原生队列槽位存储均为256 MiB，元数据另计。ReaderWriterQueue预分配16,384个512槽块；每块一个备用槽，因此可用条数为8,372,224。rigtorp请求8,388,603槽，加padding及备用槽后为256 MiB。atomic_queue请求8,388,608槽，其状态数组另计。
- 普通write，不扫描current，完整块发布（UINT32_MAX），保留现有首字节volatile读回。未修改组件或开源队列内部实现。
- 准备阶段预触碰缓冲页，再在回调持续运行时预填充到“256 MiB的80% − 本次突发量”附近。小突发也在真实积压下执行，避免把已经分配的大容量误认为已使用。
- 突发前执行业务512次热身，计时16次业务；计时发送一批消息；**立即**计时4组业务，每组16次。之后交接尾块并调整下一个突发点的积压。最后排空并校验。
- 原生队列每条发布，TwoBufQueue计时内按完整块发布，尾块在后64次业务后交接。此测试不用于比较到达回调的延时。
- 点间准备、最终排空、分配和预触碰均不计入写入及业务时间。14档共享持续运行的队列，不在每档之间强制排空。80%是目标积压上限，**不是整次遍历的累计发送量**；一遍合计发送8,388,096条正式消息，准备消息另计。
- 三次独立进程取算术平均。顺序轮换，各测试不并行。

回调循环先统一校准。默认在真实128 MiB突发下测试128/256/512次循环，对两种ReaderWriterQueue复制选项、两种rigtorp预取选项和atomic_queue均检查积压。试跑要求至少75%以留出波动余量，正式结果保留真实占用率。可以通过`--callback-iterations`固定统一循环次数。

## 五、历史诊断的指标

- **突发写入M条/s**：计时区的成功条数/耗时，包含32 B消息生成、正常写入及失败重试。
- **突发前业务ns/次**：热身后的16次业务平均耗时。
- **突发后前64次业务ns/次**：紧接突发的4组均值的平均。另列4组恢复过程。
- **相对无队列耗时倍数**：同突发量下，前64次业务均值/无队列的对应均值。无队列突发只是避免优化删除的序号循环，不显示它的写入吞吐。
- **未消费数据占用率**：（已成功写入 − 已执行回调）×32/256 MiB。包含当前源块与在途数据；不等于Reader池占用率或缓存命中率。TwoBufQueue原始文件还记录已复制但未回调的字节数。
- **背压%**：某条正式消息首次写入失败就计一条背压消息，除以正式条数。仍重试至成功，失败尝试次数另存；不丢消息。

只有3个独立突发样本，不生成p99/p999；16次业务组均值也不冒充单次调用延时。核心迁移、后台竞争、元数据、写入持续时间均可能影响业务恢复，不能只根据这些耗时断言L1未命中率或精确微观原因。

## 六、历史诊断的编译与运行

```sh
cmake -S . -B build-burst -DCMAKE_BUILD_TYPE=Release \
  -DBRQ_BUILD_SPSC_BENCHMARKS=ON -DBRQ_SPSC_DEPS=/path/to/pinned/deps
cmake --build build-burst --target test_burst_cache
build-burst/test_burst_cache --self-test
python3 benchmarks/run_burst_cache.py \
  --executable build-burst/test_burst_cache \
  --output docs/performance_data/burst_cache_local \
  --report docs/performance_burst_cache_diagnostic.html
```

例如单独测试第一层总量16 KiB、4 MiB突发、256次回调循环：

```sh
build-burst/test_burst_cache --transport two_buf_queue \
  --source_pool_bytes 16384 --burst_bytes 4194304 \
  --callback_iterations 256 --repeats 1
```

`Config`还可以修改业务表、每次访问数量、热身次数、业务计时组大小、占用目标、准备超时和各通用队列参数。独立runner使用`bursts`及`repeats`确定正式条数，通用配置中的`messages`、原分轮和旧采样参数不驱动此测试。关闭预填充可用`--prefill false`，但不再保证大积压；这一历史诊断流程使用预填充。

报告默认保留全部Writer参数、全部原生复制选项、三次独立值、初始化字段、命令与哈希。准备消息带独立序号标记；正式消息在所有实现中逐字节相同，消费侧校验完整32 B、FIFO及条数，并检查业务校验和一致。

`burst_prefault.cpp`仅链接到这个测试目标。它在aligned allocation返回、对象尚未构造时清零原始存储，提前触碰实际缓冲页；原生队列另用真实入队/出队预触碰。不会改写活对象，也不会在正式热路径插入分配或清零。

macOS使用写线程高QoS调度偏好，不能保证固定核心或禁止迁移；Linux可通过producer_cpu参数绑核。2 KiB等极小块会增加Reader对象和元数据数量，进程RSS与数据池容量分开记录。

## 七、遍历业务工作集大小

业务表大小由 `Config::business_bytes` / `--business_bytes` 控制。
默认值根据本机24、48、96 KiB对照遍历选为48 KiB。这个参数是所有队列共用的业务负载，不为不同队列设置不同大小。

```sh
python3 benchmarks/tune_burst_business.py \
  --executable build-burst/test_burst_cache \
  --output docs/performance_data/burst_business_local \
  --business-kib 24,48,96 --bursts-mib 1,2,4,8,16 \
  --runs 3 --window-repeats 8 --callback-iterations 256
```

第一层总池固定16 KiB，第二层256 MiB；ReaderWriterQueue用try_enqueue及普通复制，rigtorp用try_push且不添加写预取，atomic_queue启用SPSC。
每个条件先热身，再发送，随后观察前64次业务；每次独立进程重复8个窗口。选择时取这些窗口平均耗时的中位数，综合全部发送量和三次独立运行中相对最快开源对照的优势倍数，减少个别调度停顿影响。
输出全部原始数据及summary.json，不根据这次少量样本推算p999。各业务工作集完整结果均保留。

测试程序也支持 `--burst_sizes 1048576,2097152,4194304,8388608,16777216` 一次指定发送量列表。

## 八、只检查1 MiB发送后的尾部

```sh
python3 benchmarks/run_burst_tail_1m.py
```

固定本次48 KiB业务工作集、32 B消息、64次回调循环、16 KiB第一层和256 MiB第二层；各开源对照使用原常规API。一次运行比较TwoBufQueue三种重试策略（每1,024次、每65,536次重试主动yield，以及关闭主动yield），并运行三个开源对照。每条件每轮10,000个后业务组样本、三轮独立运行；默认保存docs/performance_burst_tail_1m.html和带时间戳的原始数据。

`retry_yield_every`现在也控制突发发送循环，0表示只用cpu_relax重试、不主动yield。该参数只影响暂时写入失败后的等待，成功写入逻辑和后业务计时窗口保持一致。默认仍为1,024，未因一次较好的筛选值而修改原参数。程序保留全部三轮数值，不删除慢样本，不承诺p999不受系统调度影响。

`--profile`指定完整初始化参数；`--executable`复用已构建程序；`--output`指定新目录，`--resume`复用参数和可执行文件哈希一致的已完成条件；`--render-only`从完整数据生成报告。固定参数文件为benchmarks/profiles/burst_tail_1m.json，其平台和编译器设置按本次macOS机器保存。
