本目录为 TwoBufQueue 普通 8 B 写入回退的诊断证据；没有生成或替换四份正式 HTML 性能报告。

实际采用的修改仅在 benchmarks/tunable/performance_test.hpp：取消 retry_until_accepted、send_latency、begin_group、produce、send_prepared、dispatch 六处 FAST_LOG_INLINE_LAMBDA 强制内联，让编译器自行选择测试模式和整轮发送循环的函数边界。Writer::write/prepare_write/commit_write 的内联和实现均保留。62 个 src/include 源文件的逐文件 SHA-256 与修正前相同；第三方队列源码和 Payload 的复制策略没有再次修改。

原强制内联版本把普通写入、Session、时钟和多种负载模式展开到 measure 大函数。取消后，Clang 18.1.8 将预先准备负载的普通发送循环放在独立函数中，而 Writer::write 仍展开在它的内层循环。每条记录仍调用 push -> writer.write，保留相同的空间检查、复制、游标更新、冷路径换块和失败重试。定时器的位置、准备输入、轮间等待/回调排空、写入数量和数据完整性检查均未改变。仅改这些编译标记的控制实验即可稳定复现收益，说明之前把回退全部归给 Writer 的双状态维护并不充分。不能从汇编或本实验直接证明某一特定 CPU 执行端口、频率或缓存硬件事件是根因。

final-control-summary.json 的同轮随机顺序完整程序对照，每个指标 5 次算术平均，包含所有低分/异常波动，未挑选最好成绩：
旧程序原样重建：852.393 M条/s，计时组平均写入耗时 p50 1.000000 ns。
当前修正前：739.048 M条/s，p50 1.125000 ns。
本次修正后：1307.190 M条/s，p50 0.916667 ns。
同轮修正后 ReaderWriterQueue：753.090 M条/s，p50 1.300000 ns。
普通写入恢复 76.9% 吞吐；这不是生产库算法增加 76.9% 的宣称，而是测试代码的编译回退修正。延时是每组 1000 次写入总时间/1000 的分位数，不是单条消息或端到端分位数。吞吐与延时为独立测试循环，不能要求吞吐严格等于 1/p50。

此前另一轮完整程序同参数验证 auto-four-length-summary.json：普通 8 B 764.328 -> 1290.492 M条/s，16 B 621.757 -> 832.969，32 B 481.885 -> 504.249，64 B 370.342 -> 371.304。单次低分全部保留，16 B 的 319.053 M条/s 修正前样本也保留，不用删除该样本夸大收益；五次原始分数见 JSON。

Session 也参与校验，写入循环和库实现未改。其吞吐受当前轮次的扰动较大；本次两个完整程序控制中，8 B 与 32 B 的五次均值有约 5～9% 降低，不能将本修正称为 Session 吞吐优化，也不能以更快的 p50 掩盖这些低分。原始逐次分数全部保留。当前定位并修正的对象是普通 8 B 写入的明显回退，尚未生成新正式报告或以单轮分数作可移植保证。

关于历史 944.273 M条/s：从 completed_blocks_8m_zero_20261010_134730 的 source-sha256.json 恢复全部 144 个源文件，其中 8 个旧文件取自 read-config-migration-before 快照。historical-exact-source-audit.json 逐一匹配；重建可执行文件 SHA-256 与当时记录完全一致：2e0958c363431e2af9fe4bb5e05964e337d18f401735a2cf409b29fae567e161。该原样旧程序在本次同参数对照仅约 852.393 M条/s，历史值不能直接当作本轮代码基线。重复控制中旧程序均值也有变化。由于 macOS 无硬绑核且没有记录逐样本 CPU 频率/迁移/硬件计数，无法进一步量化是哪种运行时因素造成了旧程序跨轮次差异；不能声称是电源、温度、L1/L2 或编译器版本改变。本修正后普通 8 B 的全部 5 次吞吐均高于历史 944 M条/s。

条件：本机接电源，producer user_interactive QoS，未硬绑核；10,000,000 次成功写入；源池与输出池各 <= 8 MiB；8 B 的每层块为 1 MiB x 8；关闭扫描 current，publish_count=UINT32_MAX；callback_iterations=0；每条完整变化数据在计时前准备；一轮约 0.8 x 8 MiB，排空后继续；循环里没有主动 flush。其他长度采用冻结的 spsc_release.json 参数，修正前后每个配置逐字段相等。开源队列使用同样修改后的测试框架，原复制配置不变。

文件说明：
final-control-*：31 个程序/队列/长度组合，310 次正式诊断测量 + 31 次完整负载/FIFO 校验。
auto-four-length-*：普通与 Session 的 8/16/32/64 B，160 次测量 + 16 次完整校验。
autoinline-*、lambda-factor-*：同库的限制实例程序，只取消全部标记或单独一处标记的归因实验。
counter-*、single-state-*、four-length-*、cursor-*、isolated-loop-*：未采用的状态重写/字段重排/独立发送辅助函数实验。仅 remaining 的方案改善 8 B，却使普通 16 B p50 从约 1.017 增至 1.617 ns；字段重排没有收益，游标差简化约 600 M条/s。均未应用到生产代码。
current-measure8.s / after-full8-loop*.s / historical-dispatch8.s：从实际完整二进制提取的汇编；counter_autoinline-*.s 为相同库的控制汇编。
before-source.tar.gz：Git 161702a 实验基线。
after-source.tar.gz：本次修正源码与额外混合 Writer/Session 校验用例。
historical-source.tar.gz：144 项哈希与旧记录匹配的历史源码。
diagnostic-variants.tar.gz：未采用的候选与受限主程序差异文件；公共文件来自 before-source。
run_*.py、counter-build-commands.json、compile_commands.json：测量/构建脚本和实际命令。脚本中的本机二进制路径需按本地重建目录调整；依赖为 benchmarks/bootstrap_spsc.py 固定版本，可复用 work/perf-deps 缓存。无需改第三方源码。
metadata.json / power.txt / machine.txt / compiler.txt：环境与源码归档身份。

普通写入、扫描 current、同步回调、Session 以及性能框架/搜索/报告契约 7 项检查全部通过；完整测量每次验证 consumed=10,000,000。完整数据校验在独立样本中开启，正式诊断样本 verify_payload=false，保留原测试语义。库属性搬移、Writer 状态精简等尝试未通过性能约束，因此没有为了达到分数去改变这些实现。
