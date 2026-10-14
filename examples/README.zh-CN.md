# 示例

[English](https://xiyang1314.github.io/TwoBufQueue/examples/README.html) · [简体中文](https://xiyang1314.github.io/TwoBufQueue/examples/README.zh-CN.html)

从仓库根目录开始阅读。先运行 [01_basic.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/01_basic.cpp.zh-CN.html)，再按需要选择下面两条路线；编号表示各目录中的阅读顺序，不要求把所有示例依次读完。

- [`two_buf_queue/`](two_buf_queue/)：完整的 Writer → 读取 → 用户回调流程。只想在应用中使用 TwoBufQueue，先看这一组。
- [`components/`](components/)：单独使用 WarmPool、SpscLiveHandoffQueue 或 HandoffQueue。只有需要自己组合这些组件时才看这一组。

## 一、先看入门示例

[01_basic.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/01_basic.cpp.zh-CN.html) 是最短的完整流程：创建 Writer 和 TwoBufQueue，写入消息，在回调中读取，最后调用 `writer.finish()` 和 `queue.stop()`。`Writer` 在调用者线程写入；读取和回调由队列的读取线程执行。读完它，再选择下表中的专题。

## 二、使用完整的 TwoBufQueue

| 示例 | 何时看 | 重点 |
| --- | --- | --- |
| [02_batch_callback.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/02_batch_callback.cpp.zh-CN.html) | 想一次回调接收一组数据 | `TwoBufQueueBatch` 把 `DataBatch` 交给回调；示例自行计算每个 frame 中的记录数。 |
| [03_multi_thread_writer.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/03_multi_thread_writer.cpp.zh-CN.html) | 多个生产线程写入 | 每个线程使用自己的 Writer；用 `thread_id` 区分来源，并设置多个第一层读取线程。 |
| [04_layer1_callback.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/04_layer1_callback.cpp.zh-CN.html) | 想直接处理源块，跳过输出块 | 自己实现 `ReadWriterDataCallback`，将回调交给 `ReadWorker`，读取已完成块与当前块。 |
| [05_seq_writer.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/05_seq_writer.cpp.zh-CN.html) | 每条记录需要序号 | 多个 `SeqWriter` 共用原子计数器，通过 `fetch_add` 分配序号；示例校验每个 Writer 的写入顺序。 |
| [06_sync_writer.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/06_sync_writer.cpp.zh-CN.html) | 要在写线程立即执行逐条回调 | 沿用 `01_basic` 的格式与回调，给 Writer 传入 `SyncContext<decltype(rcvCallback)>` 类型参数；`write` 返回时回调已完成。 |
| [07_quick_write_muti_msg.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/07_quick_write_muti_msg.cpp.zh-CN.html) | 连续写入多条记录 | 取得单块 Session，双重循环写入；空间不足时由外层取得下一块，显式选择 flush。 |
| [08_all_parameters.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/08_all_parameters.cpp.zh-CN.html) | 已了解基本用法，准备调配置 | 逐项展示 Writer 模板参数、源池与背压、读取线程和 TwoBufQueue 构造参数。 |
| [09_async_logger.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/09_async_logger.cpp.zh-CN.html) | 要把业务日志交给后台写文件 | 两层池与独立回调线程；小缓存的 Block / Drop、flush、排空退出和完整文件校验。 |

推荐路线：`01 → 02` 了解逐条消息回调与数据分组回调；需要并发时看 `03`，需要直接读源块时看 `04`。`05` 和 `08` 分别是序号与参数专题；连续写入看 `07`。
写线程内同步回调看 `06`，回调签名与 `01` 相同。
实际文件写入看 `09`；[运行说明](https://xiyang1314.github.io/TwoBufQueue/docs/guides/ASYNC_LOGGING.zh-CN.html)解释队列 flush 与文件 flush 的区别。

## 三、单独使用组件

| 示例 | 何时看 | 重点 |
| --- | --- | --- |
| [11_warm_pool.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/components/11_warm_pool.cpp.zh-CN.html) | 内存池的使用 | 实现 `reset()`，借出 `Loan`，调用 `finish()` 归还。 |
| [12_warm_pool_factory.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/components/12_warm_pool_factory.cpp.zh-CN.html) | 内存池的对象要自定义创建或销毁 | 在 `11` 的基础上提供 Factory 的 `create()` 与 `dispose()`。 |
| [13_spsc_live_handoff_queue.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/components/13_spsc_live_handoff_queue.cpp.zh-CN.html) | 结合内存池（资源池）的spsc生产者消费者队列。消费者能访问还未完成正在生产的产品 | `try_pop()` 出队；暂时无法处理时，消费者自己保存 pending 记录。 |
| [14_spsc_live_handoff_queue_claim.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/components/14_spsc_live_handoff_queue_claim.cpp.zh-CN.html) | 同上，claim/cancel代替pop | 与 `13` 对照：`try_claim()` 领取，处理失败时 `cancel_claim()` 保留队首。 |
| [15_handoff_queue.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/components/15_handoff_queue.cpp.zh-CN.html) | 结合内存池（资源池）的spsc生产者消费者队列 | SPSC 模式；队列内置 Reader 调用 `bool(Message&)` 回调，返回 `false` 后重试。 |
| [16_handoff_queue_mpmc.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/components/16_handoff_queue_mpmc.cpp.zh-CN.html) | 同上，多生产者、多消费者 | MPMC 模式；多个生产线程发送，多个内置 Reader 并发执行回调。 |

推荐路线：先看 `11`；要自定义对象生命周期再看 `12`。如果需要读取**尚在生产中的**资源，看 `13 → 14`；如果只在 `send()` 后交给消费者处理，看 `15`，需要多线程消费再看 `16`。`13/14` 自己管理消费者线程，`15/16` 由 HandoffQueue 管理 Reader 线程。

## 四、运行与理解回调

在仓库根目录构建并运行全部示例：

```sh
cmake -S . -B build -DBRQ_BUILD_TESTS=OFF -DBRQ_BUILD_EXAMPLES=ON -DBRQ_BUILD_BENCHMARKS=OFF
cmake --build build --config Release --parallel
ctest --test-dir build -C Release -R '^example_' --output-on-failure
```

只想运行一个例子，可先构建对应目标，例如 `cmake --build build --config Release --target brq_component_15_handoff_queue`。在 Ninja/MinGW 的 Windows 构建中运行 `build/brq_component_15_handoff_queue.exe`；Visual Studio 构建在 `build/Release/` 下。所有示例都是独立程序，正常结束返回 0，失败返回非零值。

阅读代码时留意三个边界：`finish()` 或归还后不能再访问资源；`DataBatch`、源块数据等借用视图只能按回调约定使用，不能在回调返回后保存指针；停止时先结束生产，再等待读取和队列排空。回调返回 `false` 的含义随接口而定，具体以各例注释为准。

[example_support.hpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/example_support.hpp.zh-CN.html) 只提供示例共用的检查与解析辅助函数，并引入 `<TwoBufQueue.hpp>`；它不是用户必须依赖的 API。组件接口可从 [WarmPool](https://xiyang1314.github.io/TwoBufQueue/docs/code/src/base_components/WarmPool/warm_pool.hpp.zh-CN.html)、[SpscLiveHandoffQueue](https://xiyang1314.github.io/TwoBufQueue/docs/code/src/base_components/LiveHandoffQueue/spsc_live_handoff_queue.hpp.zh-CN.html) 和 [HandoffQueue](https://xiyang1314.github.io/TwoBufQueue/docs/code/src/base_components/HandoffQueue/handoff_queue.hpp.zh-CN.html) 的头文件继续阅读。
