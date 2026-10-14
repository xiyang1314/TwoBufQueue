# 性能测试导航

[English](README.md) · [简体中文](README.zh-CN.md)

按测试场景选择子目录。库的默认构建不依赖性能测试组件。测试口径见[性能测试说明](../docs/reports/PERFORMANCE.zh-CN.md)，参数见[可调整的对比测试](guides/TUNABLE_SPSC.zh-CN.md)。

## 一、选择测试

| 目录 | 测什么 | 从这里开始 |
|---|---|---|
| `spsc/` | 普通 write、WriteSession 与开源 SPSC 队列对比 | [运行对比](spsc/run_release_benchmarks.py) · [遍历参数](spsc/tune_spsc.py) |
| `burst/` | 连续发送一批消息后，业务执行受到什么影响 | [运行实验](burst/run_burst_empty.py) · [测试说明](guides/BURST_CACHE.zh-CN.md) |
| `block_size/` | 遍历 writer 块大小，分别测试完整流水线和单独 writer | [完整流水线](block_size/test_per_writer_block_size.py) · [单独 writer](block_size/test_per_write_block_size_only_writer.py) |
| `hot_path/` | 生产者内存占用与业务热缓存实验 | [运行实验](hot_path/test_hot_path_cache.py) · [测试说明](guides/HOT_PATH_CACHE.zh-CN.md) |
| `tunable/` | 共用的 C++ 负载、队列适配和参数处理 | 供各测试程序使用 |
| `common/` | 共用的进程资源统计 | 供 C++ 测试使用 |
| `profiles/` | 保存的负载与参数配置 | 传给对应脚本 |
| `guides/` | 参数和实验说明 | [SPSC 对比指南](guides/TUNABLE_SPSC.zh-CN.md) |

## 二、构建测试程序

从项目根目录运行：

```sh
python3 benchmarks/spsc/bootstrap_spsc.py
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DBRQ_BUILD_SPSC_BENCHMARKS=ON
cmake --build build-bench --parallel
ctest --test-dir build-bench --output-on-failure -L performance_harness
```

依赖库需要明确下载，版本固定并校验哈希。`run_release_benchmarks.py` 运行对比并保存复现信息；`tune_spsc.py` 在给定内存预算中遍历参数；[plot_release.py](spsc/plot_release.py) 只根据保存的数据画图，不重跑测试。

统计内存池深度的块大小实验另需 `BRQ_ENABLE_POOL_DEPTH_STATS=ON`。正式性能对比使用默认未启用深度统计的构建。测试程序名称和命令行参数保持一致。

`BRQ_BUILD_BENCHMARKS` 和 `BRQ_BUILD_COMPARISON` 已停用；历史实验保留在原开发目录。
