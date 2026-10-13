# Release source copy / 发布源码副本

The release directory contains current library source and headers, CMake package support, runnable examples, active regression/functional tests, CI, current SPSC comparisons and block-size/backlog tests, published reports and their referenced evidence. LICENSE and NOTICE remain at the root.

发布副本包含现版源码、安装包配置、有效测试、全部可运行示例、中英文入口、当前性能测试程序、正式报告与其引用的复现数据。临时笔记、目录副本、`.DS_Store`、构建产物和依赖下载目录不进入上传清单。筛选不删除原项目或目标目录中的任何文件；目标目录已有的非发布文件由 Git 忽略。

业务性能的当前入口是[连续发送一批消息，对业务性能产生什么影响](performance_burst_from_empty.html)：每批从零积压开始，展示发送后业务耗时与正常耗时的比值。[测试流程与复现命令](../benchmarks/BURST_CACHE.zh-CN.md)和全部参数、原始样本一起发布。此前[预填充大积压的报告](performance_burst_cache.html)与历史图表也保留，两种起始状态分别说明。

```sh
python tools/export_release.py ~/Documents/TwoBufQueueRelease/TwoBufQueue --update
python ~/Documents/TwoBufQueueRelease/TwoBufQueue/tools/verify_release.py
```

The exporter follows local links, copies their targets and checks SHA-256 hashes. `release-manifest.json` lists each published file, size and hash. Measurement JSON/JSONL/CSV over 1 MiB is losslessly compressed in the destination, and published links are adjusted. `compression-manifest.json` records both hashes; `tools/unpack_performance_data.py` restores original paths and refuses to replace differing files. Recorded measurements, metadata and source archives are not rewritten. The exporter does not copy `.git`, create commits or publish anything. Differing destination files require `--update`. Extra destination files are never deleted.

[GitHub publishing steps](PUBLISHING.md) · [Report index](index.html).

The current supported comparison entry is `BRQ_BUILD_SPSC_BENCHMARKS=ON`. The old `BRQ_BUILD_BENCHMARKS` and `BRQ_BUILD_COMPARISON` APIs are deprecated and produce a clear configuration error. Their historical source is preserved in the development directory and excluded from the current release copy.

正式数据中的源码归档与历史绝对命令路径属于复现证据，不能清洗或重写成当前路径；使用当前[性能测试说明](PERFORMANCE.md)中的可移植命令自行重测。复制构造敏感性控制实验也保留。发布副本可以独立配置和测试，不依赖开发目录的未复制文件。
