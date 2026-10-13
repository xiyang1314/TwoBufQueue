# TwoBufQueue

**面向日志、遥测和事件采集，用小生产者缓存降低 C++20 热路径的写入开销。**

每个生产线程使用自己的缓冲区，后台线程将数据批量复制到第二层，独立回调线程负责处理。业务线程逐条写入，不执行文件 I/O；连续提交大量记录时，可以使用单块 `WriteSession`。

两层内存池的容量分别设置：生产者使用小缓存，消费者使用大缓存容纳积压。下面的实测使用 **16 KiB 生产者内存池和 256 MiB 第二层内存池**。

[English](README.md) · [CMake 接入](docs/INTEGRATION.md) · [异步日志示例](docs/ASYNC_LOGGING.md) · [性能测试](docs/PERFORMANCE.md) · [示例索引](examples/README.md)

## 数据怎样流动

```mermaid
flowchart LR
    A[业务线程] --> B[Writer：源块]
    B --> C[后台复制线程]
    C --> D[第二层输出块池]
    D --> E[独立回调线程]
    E --> F[文件 / 遥测 / 批量处理]
```

多个生产线程各建一个 Writer 和源池，保留各自顺序；回调收到的数据只在本次调用期间有效。可选直接读取路径省去第二层复制；非 void `SyncContext` 可让逐条回调在写线程执行。详细参数与所有权约定见[接口文档](docs/API.zh-CN.md)。

## 最小使用示例

```cpp
#include <TwoBufQueue.hpp>
#include <cstdio>
#include <string_view>

int main() {
    auto writer = TwoBufQueue::make_writer<TwoBufQueue::Writer<>>();
    auto on_record = [](std::int32_t, const std::byte* data,
                        std::size_t size) noexcept {
        std::printf("%.*s\n", static_cast<int>(size),
                    reinterpret_cast<const char*>(data));
    };
    TwoBufQueue::TwoBufQueue queue(on_record, writer);
    bool ok = true;
    for (std::string_view text : {"hello", "telemetry", "goodbye"})
        ok &= writer.write_mem(text.data(), static_cast<std::uint16_t>(text.size()));
    writer.flush();   // 发布并通知；不等待回调完成。
    writer.finish();  // 停止生产，交接尾块。
    queue.stop();     // 等待复制和回调排空，再结束作用域。
    return ok && queue.error_code() == TwoBufQueue::ReadError::none ? 0 : 1;
}
```

[异步日志示例](examples/two_buf_queue/09_async_logger.cpp) 演示后台写文件、小容量池受压、等待／丢弃、flush 和退出，并逐条校验保存的文件。

## 连续发送一批消息，对业务性能产生什么影响

![Apple M3 上每批发送 2、4、8、16 MiB 后的业务组均值 p50](docs/assets/burst_business_cost.svg)

生产者先正常执行业务函数，再连续发送一批 2、4、8 或 16 MiB 消息，发送完后立刻执行同样的业务函数。每批发送前积压为 0，消费者持续运行；测量完成后在计时外排空，再开始下一批，不预填消息。所有队列使用相同的 32 B 消息、48 KiB 业务数据表和 64 次回调循环。TwoBufQueue 第一层总池为 16 KiB，第二层为 256 MiB；各开源 SPSC 队列的槽位存储为 256 MiB。

正常执行业务函数约 **13 ns/次**。每批发送 16 MiB 后，TwoBufQueue 为 **18.2 ns/次**（1.40 倍），ReaderWriterQueue 为 **58.2 ns**，rigtorp 为 **42.5 ns**，atomic_queue 为 **99.8 ns**。每批 2 MiB 时，TwoBufQueue 与 rigtorp 均为 15.6 ns/次。这里展示的是允许批量处理时，队列对生产者热路径业务的影响。

环境为 Apple M3、macOS 14.4.1、Clang 18.1.8，三轮独立运行，生产者高 QoS，未硬绑核。指标为每组 16 次业务调用的平均耗时 p50，再对三轮取平均。图中虚线表示正常耗时 13 ns；报告中的比值为发送后业务耗时 ÷ 13 ns。[完整场景、参数、校验和原始样本](docs/performance_burst_from_empty.html) · [一次运行复现](benchmarks/BURST_CACHE.zh-CN.md)。

此前[预填充大积压的实验](docs/performance_burst_cache.html)及其约 198–205 MiB 积压数据仍保留，与上图的起始状态不同。

## 当前生产者写入性能对比

三份报告统一使用 256 MiB 开源槽位／第二层缓存、空回调、三轮独立运行。每份报告通过公开的吞吐与 p50/p99/p999 组合评分，在四种消息长度之间选择同一组 TwoBufQueue 参数。第一层容量另列，不与第二层混为一个参数；评分表示本机实测配置的表现。

| 写入方式 | 报告 |
|---|---|
| 单块 WriteSession | [Session 对比](docs/performance_256m_zero_session_plateau.html) |
| 普通 write，仅在块交接时发布 | [最大发布批次对比](docs/performance_256m_zero_publish_max_plateau.html) |
| 普通 write，每条记录都发布 | [每条发布对比](docs/performance_256m_zero_publish1_plateau.html) |

这里的 p50/p99/p999 是每组 **1,000 次写入平均耗时**的分位数，不是单条消息延时或回调完成时间。源码、全部次数、参数和复现命令见[性能证据](docs/PERFORMANCE.md)。

## 历史同参数写入实验

![Apple M3 上普通 write 与 WriteSession 的组均值 p50](docs/assets/write_cost_8m.svg)

图中普通 write 与单块 Session 使用相同参数。环境为 Apple M3、macOS 14.4.1、Clang 18.1.8；每次 1,000 万条、5 次取平均，回调循环次数为 0；两层数据池各 8 MiB，合计 16 MiB。本机高 QoS，**未硬绑核**。

p50 是每组 **1,000 次写入平均耗时**的分位数，不能当成单次调用或端到端延时。吞吐累计分轮发送时间，排空在计时外，申请块和换块在写入计时内。[数据来源与完整测量口径](docs/PERFORMANCE.md)。

| 条件 | 普通 write | WriteSession |
|---|---|---|
| 每层 8 MiB，回调循环 0 次 | [报告](docs/performance_8m_zero.html) | [报告](docs/performance_8m_zero_session.html) |
| 每层 256 MiB，回调工作量校准 | [报告](docs/performance_256m_callback.html) | [报告](docs/performance_256m_callback_session.html) |

报告还对比 ReaderWriterQueue、rigtorp/SPSCQueue 与 atomic_queue，保留全部正式次数、参数和命令。独立 A/B 实验表明，外部负载复制构造的变化会显著改变部分队列的成绩，不能把差异全部归因于队列算法。完整数据校验通过，也不意味着这些数字是所有平台的通用排名。

## 构建与接入

需要 CMake 3.20+、C++20 编译器与线程支持，无第三方运行时依赖。默认详细测试需要 Python 3.9+。macOS 实现需要 **14.4+ 系统和支持对应接口的 SDK**。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

直接接入本地源码：

```cmake
add_subdirectory(external/TwoBufQueue)
target_link_libraries(your_app PRIVATE TwoBufQueue::TwoBufQueue)
```

作为其他工程的子项目时，测试和示例默认关闭。安装后接入：

```cmake
find_package(TwoBufQueue 0.1 CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE TwoBufQueue::TwoBufQueue)
```

[接入文档](docs/INTEGRATION.md) 包含安装命令、可迁移安装目录和 FetchContent 用法。CMake 包版本为 0.1.0，仅用于包版本识别，不表示已创建 Release 或标签。

## 按用途选择示例

| 需求 | 示例 |
|---|---|
| 逐条写入与回调 | [01_basic](examples/01_basic.cpp) |
| 整批处理 | [02_batch_callback](examples/two_buf_queue/02_batch_callback.cpp) |
| 多个业务线程写入 | [03_multi_thread_writer](examples/two_buf_queue/03_multi_thread_writer.cpp) |
| 直接读取源块 | [04_layer1_callback](examples/two_buf_queue/04_layer1_callback.cpp) |
| 给记录加序号 | [05_seq_writer](examples/two_buf_queue/05_seq_writer.cpp) |
| 在写线程执行回调 | [06_sync_writer](examples/two_buf_queue/06_sync_writer.cpp) |
| 循环快速写多条记录 | [07_quick_write_muti_msg](examples/two_buf_queue/07_quick_write_muti_msg.cpp) |
| 调整构造参数 | [08_all_parameters](examples/two_buf_queue/08_all_parameters.cpp) |
| 后台写业务日志 | [09_async_logger](examples/two_buf_queue/09_async_logger.cpp) |

## 使用边界

- 同一个 Writer 只能由一个生产线程写入；读取线程启动前完成注册，每个源队列只交给一个读取器。跨生产者不保证全局 FIFO。
- 检查每次写入结果。缓存满时，默认 Drop 立即拒绝；Block 最多等待指定时间，也可能失败。内存池大小、队列容量和输出池分别配置。
- 默认记录使用两字节长度前缀，负载最多 65,535 字节，完整记录必须装得进一个源块。对象字节是进程内表示，不是跨机器序列化协议。
- 回调返回后不得保存数据指针。后续任务要先复制到应用自有存储；`noexcept` 回调自行记录或处理错误。
- 关闭 current 扫描后，flush 不会交付当前未完成块；尾部需要显式交接或 finish。启用扫描时，发布策略与线程调度仍会影响交付延时。
- 先停止并 join 生产线程，再逐个 `writer.finish()`，最后 `queue.stop()` 并检查错误。队列 flush 不等于回调完成或文件落盘。

适合逐条生产、后台批量处理和低提交开销场景。稀疏输入或严格单条延时需求，应另外测量端到端延时，调整发布和读取配置。[接口约定](docs/API.zh-CN.md) · [测试记录](test/READ_CTRL_TESTS.md) · [贡献说明](CONTRIBUTING.md)。Linux、Windows、macOS CI 已配置；保存的本机记录明确列出实际验证平台。

采用 [Apache-2.0](LICENSE)，版权所有 © 2026 chuan lu。见 [NOTICE](NOTICE)；[中文许可证](LICENSE.zh-CN.md) 是参考译文。
