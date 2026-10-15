# 测试导航

[English](https://xiyang1314.github.io/TwoBufQueue/test/README.html) · [简体中文](https://xiyang1314.github.io/TwoBufQueue/test/README.zh-CN.html)

按要验证的功能选择子目录。这里调整的是源码位置，CMake 构建目标和 CTest 用例名称保持一致。

## 一、目录说明

| 目录 | 验证什么 |
|---|---|
| `writer/` | Writer、WriteSession、同步回调和 writer 队列 |
| `reader/` | 当前块扫描、通知、单个与多个 writer 的读取和完成交接；[分日期的测试记录](https://xiyang1314.github.io/TwoBufQueue/test/reader/READ_CTRL_TESTS.html) |
| `queue/` | 完整队列的参数和统一消息处理 |
| `pool/` | 内存切片分配、容量与释放检查 |
| `handoff/` | 活跃资源交接、当前范围快照和并发 FIFO |
| `performance/` | 性能框架的正确性、参数搜索和报告统计；这里不发布性能成绩 |
| `support/` | 测试共用的构造辅助和头文件 |
| `func/` | 详细功能与并发契约；[用例索引](https://xiyang1314.github.io/TwoBufQueue/test/func/TEST_CASES.html) |
| `integration/` | 本地源码、FetchContent 和安装包接入 |

## 二、运行测试

从项目根目录运行：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

先[校验项目自带的固定版本对比依赖](https://xiyang1314.github.io/TwoBufQueue/docs/code/benchmarks/spsc/bootstrap_spsc.py.zh-CN.html)，再开启 `BRQ_BUILD_SPSC_BENCHMARKS=ON`，即可运行性能框架检查。完整说明见[测试指南](https://xiyang1314.github.io/TwoBufQueue/docs/development/TESTING.zh-CN.html)和[性能测试导航](https://xiyang1314.github.io/TwoBufQueue/benchmarks/README.zh-CN.html)。
