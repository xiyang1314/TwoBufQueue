# 文档导航

[English](https://xiyang1314.github.io/TwoBufQueue/docs/index.html) · [简体中文](https://xiyang1314.github.io/TwoBufQueue/docs/index.zh-CN.html) · [项目首页](https://xiyang1314.github.io/TwoBufQueue/README.zh-CN.html)

初次接入可先看接口指南和工程接入。性能结果、测试说明和代码阅读分别放在下面的目录中。

## 一、使用指南

| 文档 | 了解什么 |
|---|---|
| [接口指南](https://xiyang1314.github.io/TwoBufQueue/docs/guides/API.zh-CN.html) | 初始化、写入、WriteSession、flush 与接收回调 |
| [工程接入](https://xiyang1314.github.io/TwoBufQueue/docs/guides/INTEGRATION.zh-CN.html) | 本地源码、CMake 和安装包接入 |
| [异步日志示例](https://xiyang1314.github.io/TwoBufQueue/docs/guides/ASYNC_LOGGING.zh-CN.html) | 在应用中传递日志记录 |
| [示例导航](https://xiyang1314.github.io/TwoBufQueue/examples/README.zh-CN.html) | 按用途选择完整示例 |
| [详细介绍](https://xiyang1314.github.io/TwoBufQueue/README_DETAIL.zh-CN.html) | 性能问题、双层设计与适用场景 |

## 二、性能报告

| 目录 | 内容 |
|---|---|
| [reports/](https://xiyang1314.github.io/TwoBufQueue/docs/reports/index.zh-CN.html) | 性能报告入口与主要对比 |
| `reports/comparison/` | 普通 write、WriteSession 和开源队列对比 |
| `reports/burst/` | 连续发送消息后对业务性能的影响 |
| `reports/block_size/` | 块大小遍历与发布策略实验 |
| `reports/analysis/` | 缓存、延时和具体实现的分析 |
| [测试方法](https://xiyang1314.github.io/TwoBufQueue/docs/reports/PERFORMANCE.zh-CN.html) | 指标含义、测试条件与复现 |

报告的原始数据保留在 `performance_data/`，源码阅读页保留在 [code/](https://xiyang1314.github.io/TwoBufQueue/docs/code/index.zh-CN.html)。

## 三、开发与发布

| 文档 | 内容 |
|---|---|
| [运行测试](https://xiyang1314.github.io/TwoBufQueue/docs/development/TESTING.zh-CN.html) | 功能测试、性能框架检查与工程接入验证 |
| [发布内容](https://xiyang1314.github.io/TwoBufQueue/docs/development/RELEASE_CONTENTS.zh-CN.html) | 发布目录包含哪些文件 |
| [发布步骤](https://xiyang1314.github.io/TwoBufQueue/docs/development/PUBLISHING.zh-CN.html) | GitHub 仓库、Pages 与中英文包 |
| [参与贡献](https://xiyang1314.github.io/TwoBufQueue/CONTRIBUTING.zh-CN.html) | 提交功能和文档修改 |
| [测试导航](https://xiyang1314.github.io/TwoBufQueue/test/README.zh-CN.html) | 按组件选择测试目录 |
| [第三方对比队列](https://xiyang1314.github.io/TwoBufQueue/third_party/spsc/README.zh-CN.html) | 固定版本、原始许可证与离线构建 |
| [性能程序导航](https://xiyang1314.github.io/TwoBufQueue/benchmarks/README.zh-CN.html) | 选择性能实验和参数遍历程序 |
