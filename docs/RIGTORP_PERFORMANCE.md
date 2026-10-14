# rigtorp benchmark regression / 性能回退修正

本次修正的是测试调用端。rigtorp、ReaderWriterQueue 和 atomic_queue 的原始头文件均保持固定版本与原始 SHA-256。

普通 Payload 复制构造保持 `bytes(other.bytes)`。新增带 `DestinationWritePrefetch` 标签的构造，先提示目标地址将被写入，再进行同样的一次完整成员复制。rigtorp 适配器通过原始 `try_emplace` API 请求它；没有增加写零、减少消息字节或更改 release/acquire 发布。

`PerformanceConfig::rigtorp_write_prefetch` 默认为 true，CLI 可设 `--rigtorp_write_prefetch false`。测试开始前选择 `RigtorpAdapter<Payload, true/false>` 模板，写循环中没有这个开关的运行时判断。ReaderWriterQueue、atomic_queue 和 TwoBufQueue 的原有调用不变。

## Same-executable control / 同一程序开关对照

同一个优化程序、相同完整变化负载、每次 1,000 万条，吞吐和每组 1,000 次写入的平均耗时分位数各测 5 次；两种开关状态各做完整负载/FIFO 校验。8 MiB 回调循环为 0；256 MiB 按长度固定为 1/1/3/5 次，与修正前那轮校准保持一致。正式四份报告再次独立校准，并保留各次结果。写线程高 QoS，未硬绑核，接电源。

| 缓存 | 消息 | 关闭吞吐 M条/s | 开启吞吐 M条/s | 关闭 p50 ns | 开启 p50 ns |
|---|---|---:|---:|---:|---:|
| 8 MiB | 8 B | 44.51 | 345.24 | 22.083 | 2.758 |
| 8 MiB | 16 B | 43.48 | 349.74 | 23.192 | 2.800 |
| 8 MiB | 32 B | 43.90 | 303.25 | 22.625 | 3.142 |
| 8 MiB | 64 B | 50.90 | 221.58 | 19.242 | 4.283 |
| 256 MiB | 8 B | 47.97 | 313.66 | 19.475 | 3.067 |
| 256 MiB | 16 B | 44.70 | 266.82 | 21.492 | 3.783 |
| 256 MiB | 32 B | 39.36 | 162.59 | 25.683 | 6.000 |
| 256 MiB | 64 B | 39.84 | 98.23 | 24.717 | 9.883 |

全部 160 次正式测量和 16 次完整校验通过。[汇总](performance_data/rigtorp_prefetch_20261011/selective-control/summary.json) · [原始记录](performance_data/rigtorp_prefetch_20261011/selective-control/raw.jsonl) · [源码和复现脚本](performance_data/rigtorp_prefetch_20261011/selective-control/source.tar.gz)。

## Code generation and limits / 汇编与结论边界

保存的 16 B 推送汇编中，两个路径均为一条 `ldr q0`、一条 `str q0` 和相同的 `stlur` 发布；开启路径在复制前增加 `prfm pstl1keep`，没有额外清零存储。[汇编](code/docs/performance_data/rigtorp_prefetch_20261011/push_codegen.s.html)。

`__builtin_prefetch(destination, 1, 3)` 表示为写操作给出高时间局部性的预取提示，它不写入消息内容；这是性能提示，不保证每个平台都会提速。[GCC 官方说明](https://gcc.gnu.org/onlinedocs/gcc/Other-Builtins.html#index-__builtin_prefetch)。不支持该内建函数的编译器使用普通完整复制。

对照可以确认：本机此调用路径的回退能够通过目标写预取消除，不需要恢复函数体 memcpy 的额外写零。具体 M3 内部流水线或缓存所有权机制尚未由硬件计数器证实，不据此推断所有队列或平台的通用排名。

固定队列对象偏移 0/64（mod 128）的对照均没有改善该回退，因此未采用对齐改动。[排除布局假设的记录](performance_data/rigtorp_prefetch_20261011/placement-control/summary.json)。

前期把预取统一加入普通 Payload 复制时，rigtorp 提升，但 8 B ReaderWriterQueue 吞吐从约 766 降到 481 M条/s，因此未采用这个版本。[统一添加提示的对照](performance_data/rigtorp_prefetch_20261011/fix-control/summary.json)。该修正只在 rigtorp 适配器显式请求提示。后续 ReaderWriterQueue 16/32 B 的调用端复制调整单独披露于 [性能口径](PERFORMANCE.html)，不属于这次预取开关对照。

## Fresh formal reports / 修正后的完整复测

最新四份报告用提交 `f698f25` 重新测量：400 次独立正式测量、40 次完整负载/FIFO 校验、45 次回调校准全部完成。8 MiB / 16 B 的 rigtorp 五次平均为 317.500 M条/s，分组平均写入 p50 为 3.017 ns。256 MiB 的回调循环按 8/16/32/64 B 固定为 1/1/2/4；校准时间比约 3.09/2.17/1.95/2.38，其中 8 B 与 32 B 略超出目标。此次改动是取消测试框架 lambda 的强制内联，没有再次修改 rigtorp 预取或第三方头文件。同缓存普通 write / Session 共享新测的开源结果；上方历史开关对照仍保留，不能把不同轮次的全部差异归因于预取。

[8 MiB 普通 write](performance_8m_zero.html) · [8 MiB Session](performance_8m_zero_session.html) · [256 MiB 普通 write](performance_256m_callback.html) · [256 MiB Session](performance_256m_callback_session.html) · [全部原始记录](performance_data/release_20261011_inline_fixed_110024/raw.jsonl.gz) · [运行清单](performance_data/release_20261011_inline_fixed_110024/manifest.json)。
