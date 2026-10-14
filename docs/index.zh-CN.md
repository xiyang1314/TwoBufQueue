# 文档导航

[English](index.md) · [简体中文](index.zh-CN.md) · [项目首页](../README.zh-CN.md)

初次接入可先看接口指南和工程接入。性能结果、测试说明和代码阅读分别放在下面的目录中。

## 一、使用指南

| 文档 | 了解什么 |
|---|---|
| [接口指南](guides/API.zh-CN.md) | 初始化、写入、WriteSession、flush 与接收回调 |
| [工程接入](guides/INTEGRATION.zh-CN.md) | 本地源码、CMake 和安装包接入 |
| [异步日志示例](guides/ASYNC_LOGGING.zh-CN.md) | 在应用中传递日志记录 |
| [示例导航](../examples/README.zh-CN.md) | 按用途选择完整示例 |
| [详细介绍](../README_DETAIL.zh-CN.md) | 性能问题、双层设计与适用场景 |

## 二、性能报告

| 目录 | 内容 |
|---|---|
| [reports/](reports/index.zh-CN.md) | 性能报告入口与主要对比 |
| `reports/comparison/` | 普通 write、WriteSession 和开源队列对比 |
| `reports/burst/` | 连续发送消息后对业务性能的影响 |
| `reports/block_size/` | 块大小遍历与发布策略实验 |
| `reports/analysis/` | 缓存、延时和具体实现的分析 |
| [测试方法](reports/PERFORMANCE.zh-CN.md) | 指标含义、测试条件与复现 |

报告的原始数据保留在 `performance_data/`，源码阅读页保留在 [code/](code/index.zh-CN.md)。

## 三、开发与发布

| 文档 | 内容 |
|---|---|
| [运行测试](development/TESTING.zh-CN.md) | 功能测试、性能框架检查与工程接入验证 |
| [发布内容](development/RELEASE_CONTENTS.zh-CN.md) | 发布目录包含哪些文件 |
| [发布步骤](development/PUBLISHING.zh-CN.md) | GitHub 仓库、Pages 与中英文包 |
| [参与贡献](../CONTRIBUTING.zh-CN.md) | 提交功能和文档修改 |
| [测试导航](../test/README.zh-CN.md) | 按组件选择测试目录 |
| [性能程序导航](../benchmarks/README.zh-CN.md) | 选择性能实验和参数遍历程序 |
