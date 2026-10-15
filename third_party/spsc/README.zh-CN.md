# 性能测试使用的第三方队列

[English](https://xiyang1314.github.io/TwoBufQueue/third_party/spsc/README.html) · [简体中文](https://xiyang1314.github.io/TwoBufQueue/third_party/spsc/README.zh-CN.html)

这里保存 SPSC 对比测试需要的固定版本头文件和许可证，随仓库、源码包和中英文发布包一起提供。构建性能测试不需要再下载这些依赖；正常使用 TwoBufQueue 库也不依赖它们。

## 一、包含的版本

| 上游项目 | 版本 | 固定提交 | 许可证 |
|---|---|---|---|
| [cameron314/readerwriterqueue](https://github.com/cameron314/readerwriterqueue) | v1.0.6 | `8e7627d18c2108aca178888d88514179899a044f` | BSD-2-Clause; embedded zlib |
| [rigtorp/SPSCQueue](https://github.com/rigtorp/SPSCQueue) | commit | `1053918dbd251fbff69b24ef27fa5d51c29ec2af` | MIT |
| [max0x7ba/atomic_queue](https://github.com/max0x7ba/atomic_queue) | commit | `74fb62f86e6c3b0db824932a5c5e4bfb0125e2cc` | MIT |

这里只包含测试需要的文件，并非上游仓库的完整副本。原始头文件、版权声明和许可证保持原样；ReaderWriterQueue 的 `atomicops.h` 中还保留了信号量实现的 zlib 许可证。

完整文件列表、来源地址和 SHA-256 校验值见 `benchmarks/spsc/spsc_dependencies.json`。本次只是把已有测试依赖放入项目，没有改变上游版本或测试负载。

## 二、离线构建

在项目根目录执行：

```sh
python3 benchmarks/spsc/bootstrap_spsc.py --check-only
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DBRQ_BUILD_SPSC_BENCHMARKS=ON
cmake --build build-bench --parallel
ctest --test-dir build-bench --output-on-failure -L performance_harness
```

CMake 默认从 `third_party/spsc/` 读取依赖，并校验每个头文件和许可证。仍可通过 `-DBRQ_SPSC_DEPS=/依赖目录的绝对路径` 指定其他目录，校验规则相同。

## 三、恢复与升级

如果误删了文件，可运行 `python3 benchmarks/spsc/bootstrap_spsc.py`，从固定提交下载缺少的文件。已有文件的校验值不一致时会报错，不会覆盖。使用 `--check-only` 只校验本地文件，不访问网络。

主动升级第三方版本时，需要一起更新依赖清单中的提交号、地址、校验值和对应的原始文件，保留许可证，然后重新构建并校验对比测试。历史性能数据不会随依赖升级而改写。

实验选择与复现方式见[性能测试导航](https://xiyang1314.github.io/TwoBufQueue/benchmarks/README.zh-CN.html)。
