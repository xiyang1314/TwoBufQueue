# 贡献指南

[English](https://xiyang1314.github.io/TwoBufQueue/CONTRIBUTING.html) · [简体中文](https://xiyang1314.github.io/TwoBufQueue/CONTRIBUTING.zh-CN.html)

项目只维护一套源码。源码注释、公开诊断与命令行提示使用英文；中英文文档分别维护。贡献者直接修改这套源码并提交 PR，不需要维护中文和英文两份实现。修改对应语言的文档即可，另一版本可由维护者协助同步。

报告问题时，请提供最小复现程序，并说明操作系统与架构、编译器版本、提交号、源块和输出块的大小与数量、Writer 发布配置、读取配置、回调行为、预期结果及实际结果。涉及资源生命周期时，请写明停止顺序，以及数据由谁持有、何时归还。不要附带私有业务数据。

提交改动前，构建默认目标并运行全部当前测试与示例。需要 C++20、CMake 3.20+、Python 3.9+；macOS 最低 14.4。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

修复行为问题时，请增加针对性的回归测试，并在提交说明中写明实际运行的目标与尚未覆盖的范围。不要把共享 CI 上的性能阈值当作回归判据。性能优化请附相同条件下的前后命令、环境信息和原始数据，参照[性能对比方法](https://xiyang1314.github.io/TwoBufQueue/benchmarks/guides/TUNABLE_SPSC.zh-CN.html)，并说明内存、线程与延时的取舍。

保持回调确认与资源所有权约定，以及每个生产者内部的顺序。不要在热路径中无说明地引入分配、阻塞或改变停止语义。公共 API 改动应同步更新可运行示例与中英文入口文档。类型、函数和数据字段命名遵循[命名规范](https://xiyang1314.github.io/TwoBufQueue/NAMING.zh-CN.html)。
