# 热路径小数据池与后台积压测试

这个独立测试展示 TwoBufQueue 的两层结构：业务写线程重复使用较小的第一层池，复制线程把消息搬到固定 256 MiB 的第二层，回调在线程中消费。它不替代原有纯写入测试，也不修改上游 SPSC 实现。

## 一、构建与复现

依赖由现有 `bootstrap_spsc.py` 下载并校验，CMake 不自动访问网络。

```sh
python3 benchmarks/bootstrap_spsc.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DBRQ_BUILD_SPSC_BENCHMARKS=ON -DBRQ_ENABLE_POOL_DEPTH_STATS=OFF
cmake --build build --target test_hot_path_cache
build/test_hot_path_cache --self-test
python3 benchmarks/test_hot_path_cache.py \
  --executable build/test_hot_path_cache \
  --output docs/performance_data/hot_path_cache_local \
  --report docs/performance_hot_path_cache_local.html
```

本机正式数据使用 Clang 18.1.8、Release `-O3 -DNDEBUG`、高 QoS，未硬绑核。Linux/Windows 可通过已有 `--producer_cpu` 设置写线程亲和性；复制/消费线程独立配置。Python 驱动目前固定 32 B 消息和 256 MiB 后台容量，所有生效参数均保存在每次 JSON 中。其他平台应重新测量，不能套用本机数值。

## 二、结构

`hot_path_cache.hpp` 中的 `Config` 包含业务模型、回调场景、测量方式，以及现有 `PerformanceConfig` 全部可调整的队列参数；`HotPathCacheTest(Config)` 构造并运行独立测试。`test_hot_path_cache.cpp` 提供 CLI。`test_hot_path_cache.py` 串行执行参数筛选、独立确认、原始数据保存和 HTML 生成。

业务模型是一个打乱顺序的链式循环，每节点 64 B，执行依赖读取与更新。`Business::step` 禁止内联，所有队列调用同一个编译后的函数，减少队列模板对业务代码生成的影响。额外提供纯整数计算对照。32 B 消息包含序号、业务结果和两个变化字段；独立校验在计时前生成完整参考消息，正式测量核验顺序、确定字段、条数和整体校验和。

正式实验每项 1,000 万条、3 次独立进程，顺序打乱。每轮统一最多写入第二层总容量的 80%（6,710,886 条），轮末交接与回调排空后开始下一轮。TwoBufQueue 使用普通 `write`、最大发布批次、关闭 current 扫描；源/输出块等大，避免暂停回调时输出块提前提交造成名义容量与可积压数据量不一致。源池固定 4 块，输出池始终合计 256 MiB。两轮真实管道预热不计时。

三种回调条件：暂停本轮首次回调直至热路径写完；每条执行同一个 16 次计算循环；计算循环为 0。0 表示没有模拟回调计算，必要的校验仍执行。TwoBufQueue 的复制线程与回调线程保持独立。

吞吐运行没有逐条时钟。延时运行每组连续执行 1,000 次业务迭代，记录组耗时除以组内次数的分位数，不能视为单次 write 或投递延时。最后不足 1,000 次的组不参与分位数，但保留计数和全部消息。另有 `--measurement sampled` 稀疏随机间隔采样诊断，记录业务本体及整次迭代原始时间；本机 41.7 ns 时钟量化限制在原始 JSON 中保存。

写线程侧数据区是队列数据池容量上限，不是 L1 占用，也不是整个进程 RSS。TwoBufQueue 的总数据池为第一层加 256 MiB 第二层；原生 SPSC 数据区约 256 MiB。元数据和业务存储另外记录或列明。没有新的硬件 PMU 采样，因此报告不把速度差异直接归因于某种缓存机制。

## 三、参数筛选与数据

TwoBufQueue 在相同业务场景下遍历块大小，四项内部权重为吞吐 35%、p50 30%、p99 20%、p999 15%，按各项最好方向归一化；要求相邻候选支持，取成绩在最高值 2% 内的最小源池，用于展示容量解耦。原生队列的原有复制选项都保留并复测，分别在缓存敏感与纯计算负载下选择三次平均吞吐更好的选项，两个业务负载之间不复用选择结果。筛选后进行独立三次对比，三个回调场景保持同一组队列参数。

报告没有使用场景或调优建议。HTML 展示优势及对照结果；`summary.json`、`results.csv`、`manifest.json` 和 `raw/*.json` 保留所有结果、精确命令、参数、校验、各次重复值、文件签名及探索性测量。驱动可断点继续，但可执行文件或命令签名改变时拒绝复用旧数据。
