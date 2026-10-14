# read_ctrl 功能测试

测试使用真实 Writer、LiveHandoffQueue 和 WarmPool，通过公开接口观察数据、游标、队列和统计。
Callback 是普通模板类型，不继承样例类。失败时打印源文件、行号及断言，单个 CTest 用例可独立运行。

| 文件 | 用例数 | 验证内容 |
|---|---:|---|
| `test_read_item.cpp` | 5 | 连续空扫描 5/10/20/30 ms；提前唤醒不改变状态；有数据恢复初始间隔；force；来源独立；时间及乘法溢出边界 |
| `test_read_service.cpp` | 14 | 空记录与延迟 begin；完成块优先；逐块确认；任意 false 停止整轮；无失败优先重试；全部来源固定快照；current 单次范围；无论成功失败均更新时间；取消、期限、来源注册、Writer 寿命和统计去重 |
| `test_read_active_completion.cpp` | 4 | 回调中完成当前块（成功/失败）；快照后追加交接记录；单块池背压及同地址新代号复用 |
| `test_reader_cohesion.cpp` | 9 | 3 Writer 并发即时/批量发布；单块池；current 未 send 即可读取；空间通知重试；快退后重启；慢退超时与打断；配置校验 |
| `test_read_notifications.cpp` | 10 | 来源定向读取、重复及多来源通知合并、完成块优先、全量扫描与通知重合及拒收、批量尾部 flush、回调中再次通知、超过 64 个来源、背压/取消保留、空间重试及注册后重启 |
| `test_single_reader.cpp` | 18 | 独立构造和配置校验、完成块优先、拒收及 pending 转交、current 单次快照、回调中通知、取消/期限、换块竞态、固定前缀、单块池并发、flush/finish 与空间重试、超时读取、快退重启、慢退超时及打断 |
| `writer_queue_tests.cpp`、`test_unified_data.cpp` | 2 | 外部池与队列寿命、Writer/ReadService 集成、缓冲存储拒收重试、消息边界、复制后源块回收 |

来源通知的 10 项回归通过 `ctest -R '^read_ctrl_notification_'` 单独运行。
单 Writer 的 18 项回归通过 `ctest -R '^read_ctrl_single_'` 单独运行。
下面带日期的结果保留当时的测试范围。原有 `ctest -L read_ctrl` 包含 34 项；完整顶层回归另含 6 项独立 LiveHandoffQueue 测试、
Writer/切片回归和 7 个示例。旧的 `handoff_seq_current_batch` 整批失败语义由新的逐块确认用例替代；
`handoff_seq_completed_priority` 由 `read_ctrl_service_completed_priority` 替代。

时间规则直接向 ReadItem 传入可控时刻，不依赖 sleep 猜测毫秒时序。
换块竞态由原子同步门控制，成功、拒收和快照后追加各覆盖 30 轮。
并发测试校验每个来源的所有序号，确保无丢失、重复或乱序；每轮加入拒收并检查全部资源最终归还。
线程测试的超时仅防止死锁，不作为性能或 p99 结论。

## 直接 pop 改动验证（2026-10-06）

Windows、MinGW GCC 15.2.0、Debug，启用 `-Wall -Wextra -Werror`，本次选定的 46 项全部通过：

- 6 项 LiveHandoffQueue：直接 pop 释放容量、资源保留到 finish、连续 pop 后按序 finish、
  过期凭证识别、池限制、50,000 条并发 FIFO（含本地 pending 重试）及发布窗口。
- 32 项 read_ctrl：含完成块拒收后持有 pending、阻塞时追加记录、固定前缀、
  逐块确认、当前块竞态、快退后重启及超时后向新 Service 转交 pending。
- `writer_queue_tests`、`test_writer` 两项 Writer 回归。
- 两个顺序队列示例、基本示例、多 Writer/Reader 示例、全参数示例共 5 项。
- `handoff_queue_cancel_two_buf`：输出 HandoffQueue 与源 pop 队列的组合。

`queue.pending_count()` 现在不包含已 pop 的记录；Reader 的 pending_finished_count() 仍包含它们。
未运行使用旧 BigMemoryStore / ReadCallback 等接口的 `test_unified_data`、
`test/func/source_lifecycle` 及其他历史套件；本次结果不代表全量测试或性能测试。

## 构建与运行

在组件根目录运行（Visual Studio 等多配置生成器同时指定 `--config Debug` / `-C Debug`）：

```sh
cmake -S . -B build-read-debug -DCMAKE_BUILD_TYPE=Debug -DBRQ_BUILD_TESTS=ON -DBRQ_BUILD_FUNCTIONAL_TESTS=OFF -DBRQ_BUILD_BENCHMARKS=OFF
cmake --build build-read-debug --parallel 4
ctest --test-dir build-read-debug -L read_ctrl --output-on-failure
ctest --test-dir build-read-debug --output-on-failure
```

将 Debug 与构建目录替换为 Release 可验证优化构建。测试断言在两种构建中均开启。
例如只运行快照后追加的竞态：

```sh
ctest --test-dir build-read-debug -R '^read_ctrl_handoff_prefix_append$' --output-on-failure
```

`BRQ_BUILD_FUNCTIONAL_TESTS=OFF` 仅关闭历史 `test/func` 跨模块套件；上述新测试由
`BRQ_BUILD_TESTS=ON` 构建。历史套件及 benchmarks 仍有旧回调 API，尚未全部迁移，
本次结果不代表它们或其他操作系统已经通过。

## 本次结果（2026-10-04）

Windows、MinGW GCC 15.2.0、Ninja，编译启用 `-Wall -Wextra -Werror`：

| 构建 | read_ctrl（含集成） | 全部顶层回归与示例 |
|---|---:|---:|
| Debug | 34 / 34 通过 | 51 / 51 通过 |
| Release | 34 / 34 通过 | 51 / 51 通过 |

Debug 分为核心测试与其余回归两批运行；重复验证的线程用例不重复计数。
本次没有发现需要修改生产逻辑的问题；修改的是测试、CTest 注册和相关说明。
未运行历史 `test/func` 套件、性能基准或其他操作系统测试。

## 独立单 Writer 实现验证（2026-10-08）

macOS arm64、Homebrew Clang 18.1.8、Release：60 项读端测试、7 项 LiveHandoffQueue 和全部 13 个示例，共 80 项通过。
单 Writer 新增 18 项；原多 Writer 通知 10 项保持通过。Debug + AddressSanitizer / UBSan 的 Service、交接竞态、Worker、通知和 single 共 55 项通过。
未运行仍引用已移除接口的 `test_unified_data` 或历史 `test/func` 套件，也未验证 Windows / Linux。

本机全示例构建使用 Homebrew libc++18（提供部分示例使用的 `std::jthread`），最低 macOS 14.4，
并以 `-Wno-error=unused-function` 允许既有 ARM 时间实现中的未使用函数警告。没有为此修改库代码。
性能数据与复现命令见 `benchmarks/SINGLE_READER.zh-CN.md`。

## 当前接口的发布检查

默认构建现在包含顶层回归、全部示例和有效的 functional 套件。旧 `review_20260925/repro.cpp` 保留为历史诊断，不再通过当前 CMake 编译；它引用的 QueueReader、ReadBatch 和 CurrentRangeReader 已被替换。

| 历史检查 | 当前有效覆盖 |
|---|---|
| address / address_multi / address_control | `test_writer` 的 true_argument_addresses：重载 operator&，单参数、多参数、write_mem 对照，逐字节校验 |
| replace / pending_replace / restart_control | `read_ctrl_service_registration`、`read_ctrl_service_writer_lifetime`、`read_ctrl_service_generation_stats`、`read_ctrl_single_restart_current`；运行期间追加 Writer 已不属于受支持接口 |
| retry_boundaries | `read_ctrl_service_retry_reclassified`、通知的 rejected_and_cancelled / full_scan_rejection_retains_notifications、source_lifecycle_buffered_retry_without_release_notify |
| stop_deadline / stop_fast | worker / single 的 slow_stop_timeout、fast_stop_restart、interrupt_slow_stop；零/负 finish_timeout 由 validation 拒绝 |
| interval / interval_boundaries | `read_ctrl_item_saturation`、empty_backoff / early_scan / data_resets、worker / single 的 validation |

DataBatch 现在是借用视图。并发管道测试通过 OutputQueue 的 loan 持有输出块，在四个处理线程上验证完整字节、每个来源的 FIFO、恰好一次处理，再归还 loan；直接模式先复制到应用自有数据。短负载不再受已经移除的 minimum 配置限制，`W_short_payload` 验证 3 字节消息被完整接收。`support/joining_thread.hpp` 仅用于测试与示例的析构 join，兼容尚未提供 std::jthread 的 Apple libc++；不改变生产者热路径。

2026-10-11，macOS 14.4.1 / Apple M3 的实际验证：

| 构建 | 通过 / 总数 |
|---|---:|
| 干净源码、默认 Release（Apple Clang） | 564 / 564 |
| 当前源码、默认 Release（Apple Clang） | 564 / 564 |
| Debug、ASan / UBSan（Apple Clang） | 564 / 564 |
| Release、启用 SPSC 对照（Clang 18.1.8） | 567 / 567 |

默认构建无需关闭 functional 测试或降低警告级别。Linux、Windows、macOS 的 CI 已配置，但云端任务尚未执行；以上通过数只表示本机结果。保存的日志和检查摘要见 [发布验证数据](../../docs/performance_data/release_20261011/release-checks/summary.json)。性能测试复现脚本也通过小规模运行和续跑检查，续跑保留原始数据、运行命令和源码快照。

同日完成首页和接入工作后，从筛选后的发布源码副本重新构建：默认测试 **567 / 567 通过**，包含新增异步日志示例及其慢回调 Block / Drop 用例。独立应用通过 add_subdirectory、本地 FetchContent、搬移安装目录后的 find_package 三种方式，每种校验 1,000 条记录。安装头文件与 CMake 导出文件没有开发源码或构建目录的绝对路径。该轮未重跑性能测量，生产 C++ 和主表 Payload 拷贝方式与原正式测量一致。[本轮检查记录](../../docs/release-checks/summary.json)。
