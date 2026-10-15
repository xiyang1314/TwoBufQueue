# 测试与验证

[English](https://xiyang1314.github.io/TwoBufQueue/docs/development/TESTING.html) · [简体中文](https://xiyang1314.github.io/TwoBufQueue/docs/development/TESTING.zh-CN.html)

当前独立 Release 构建包含库回归、functional 套件、可运行示例和接入检查。英文文档准备过程中，只包含发布清单文件的独立副本在本机 macOS 上通过 **567 / 567 项测试**。这是正确性检查，不是新性能成绩，也不代表 Linux、Windows 已实际运行。

## 一、运行当前完整测试

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

覆盖 Writer 和 WriteSession 的字节、边界、发布、背压与生命周期；读端的完成块优先、current 快照、定向通知、拒收重试与退出；内存池及块资源的所有权、对齐、代号、归还和回收；SPSC / MPMC 交接与并发完整性；完整双层流程和全部有效示例。

每个用例独立注册 CTest。通过一个子集或矩阵中的一项不等于完整套件通过。线程测试的超时用于防止死锁，不能当作性能或尾部延时结论。

## 二、单独检查一部分

```sh
ctest --test-dir build -C Release -L read_ctrl --output-on-failure
ctest --test-dir build -C Release -R '^read_ctrl_notification_' --output-on-failure
ctest --test-dir build -C Release -R '^read_ctrl_single_' --output-on-failure
ctest --test-dir build -C Release -L catalog --output-on-failure
```

机器可读 cases.json 与对应 Markdown 表格保持一致，其中历史场景文字保留原中文。测试标识和实际可运行代码共用一份，源码注释和诊断使用英文；它们不是另一套中文实现。

## 三、历史记录与平台范围

[读端历史检查](https://xiyang1314.github.io/TwoBufQueue/test/reader/READ_CTRL_TESTS.html)按日期区分当时覆盖范围。[发布检查记录](../release_checks/summary.json)和[此前验证数据](../performance_data/release_20261011/release_checks/summary.json)保留实际平台、命令和限制，不能把旧子集数量当作今天的完整测试数量。

Linux、Windows、macOS 及 sanitizer / 可选开源 SPSC 对照的 CI 已配置，远程执行要等仓库发布并实际运行。文档和界面翻译不改变库的非注释 C++ 内容，也不修改历史测量的源码快照。
