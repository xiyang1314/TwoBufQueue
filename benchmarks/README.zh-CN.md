# 当前性能测试入口

[性能证据与四份报告](../docs/PERFORMANCE.md) · [详细参数和复现命令](TUNABLE_SPSC.zh-CN.md)

当前有效入口为 `BRQ_BUILD_SPSC_BENCHMARKS=ON`：TwoBufQueue 普通 write / WriteSession 与 ReaderWriterQueue、rigtorp/SPSCQueue、atomic_queue 同负载对照。依赖仅供性能测试使用，通过 `bootstrap_spsc.py` 显式下载、固定版本并校验哈希；默认构建和库运行不依赖它们。

`run_release_benchmarks.py` 执行固定参数对照并保存完整复现证据。`tune_spsc.py` 在用户指定的预算和平台条件下搜索参数。`plot_release.py` 从已保存的正式数据重新生成首页图，不运行测试。

`BRQ_BUILD_BENCHMARKS` 与 `BRQ_BUILD_COMPARISON` 属于旧接口，已明确弃用。历史实验保留在开发目录，不作为当前结果或发布副本的测试入口。
