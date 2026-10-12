# 接口与使用约定

[中文首页](../README.zh-CN.md) · [CMake 接入](INTEGRATION.md) · [异步日志](ASYNC_LOGGING.md)

## 必须理解的几个约定

- 一个 `Writer` 直接使用外部 `WriterQueue` 和 `WriterPool`；多个生产者各建一组，并使用不同线程编号。读取器启动前完成注册。同一读取器串行调用回调；跨生产者不保证全局顺序。
- 快速路径逐块调用模板回调：完成块使用 `block->readable()`，current 使用传入的地址和长度，`source_id()` 区分 Writer。缓冲输出的 `DataFrame` 是 ReadBlock 内一个源范围的临时视图，**不等于一条消息**。默认 `Writer<>` 的记录为“两字节小端负载长度 + 负载”，长度不包含前缀，负载最多 65535 字节；`Writer<..., false>` 不写长度前缀。记录必须装得进源块。
- `write(...)` 顺序紧密复制可平凡复制的值，`write_mem(data,len)` 接收动态字节。对象表示不是跨机器序列化协议；读取未对齐数据使用 `memcpy`，不要直接强转结构体指针。
- 源回调返回 `true` 后，ReadCtrl 立即推进该块游标并归还完成块；`false` 停止本轮读取，下轮重新准备，不保存失败块或冻结范围。不得保留源指针、自行 consume 或归还。需要延后处理时先复制。`noexcept` 回调内部处理错误，并有界返回。
- `DataBatch` / `DataFrame` 是输出块中的临时视图；回调返回后不得保存其数据指针。整批回调返回 true 后由 HandoffQueue 归还整块，false 保留重试。带前缀的 Writer 用户可选择 `make_record_callback<WriterT::DataLen>`，单条成功才推进块内消费位置。
- 普通 `Writer` / `SeqWriter` 使用异步管道，`TwoBufQueueOptions::output_mode` 默认 MPSC：复制线程把输出块入队，由独立回调线程消费。其余输出读取参数原样传入 HandoffQueue；多个输出回调线程需要 SPMC/MPMC，回调自身须支持并发。逐条写线程同步回调通过下面的非 void `TSyncContext` 类型参数选择；普通 Writer 搭配 `output_mode=Sync` 会抛出 `std::invalid_argument`，避免把复制线程回调误认为写线程回调。`TwoBufQueueBatch` 的 `output_mode=Sync` 保持整批回调在复制线程执行的组件语义。
- 收尾顺序：停止并 join 写线程 → 每个 writer `finish()` → `queue.stop()`（直接使用 `ReadWorker` 时调用 `reader.stop()`）→ 检查错误并等待应用侧任务结束。手动组合 HandoffQueue 时，需保持消费运行直到队列排空；超时不代表读完，`flush()` 不等于 `finish()`。

如需 1 写 N 处理，应在回调返回前将数据复制到应用自有存储，再分发任务；不能只分发 DataBatch 或源块指针。当前接口不会自动广播，也不支持多个线程同时写同一个 Writer。

`Writer` 默认写长度前缀（`Writer<..., false>` 写原始字节）；先创建 WriterPool，再 make_writer_queue(pool, capacity, enable_stats)，最后 Writer(&queue, id, backpressure)。队列容量默认 256、统计默认关闭，独立于池上限；背压配置和统计由 Writer 持有。详见 [顺序队列](../src/base_components/LiveHandoffQueue/spsc_live_handoff_queue.hpp) 和 [逐块模板回调](../src/read/read_layer1/read_writer_data_callback.hpp)。ReadBlock 容量必须能容纳完整源范围；frame 容量、队列容量与池上限分别设置。多个 read_ctrl 各建一个 ToReadBlock，可引用同一 MPSC 输出队列；同一 Writer 的源队列只能交给一个 read_ctrl。默认连续空扫描后的等待间隔依次为 **5、10、20、30、30 ms**（从第二次空扫描起翻倍并封顶）；扫描到数据时恢复初始间隔。每次实际扫描都更新时间，与回调成功或失败无关；完成块可提前通知。调短扫描间隔或增加自旋会影响 CPU 消耗，并不保证系统有相同精度的唤醒。

单 Writer 和多 Writer 保持 Worker → Service → 回调的框架，分别在 `read_layer1/single_writer` 和 `read_layer1/muti_writer` 中实现。`SingleReadWorker(config, callback, writer)` 构造时直接绑定唯一 Writer，使用独立的 `SingleReadService`；通知只设置事件，读取路径不使用来源位图、来源容器或 Writer 数量判断。事件唤醒直接读取完成块和 current，超时按 `ReadItem` 的间隔扫描；背压、游标确认、快退/慢退和 pending 转交语义与多 Writer 相同。绑定后不支持追加 Writer，回调和 Writer 的寿命须覆盖 Worker，启动/停止时须停止并发生产。

`TwoBufQueue(callback, writer)` 和 `TwoBufQueueBatch(callback, writer)` 的单 Writer 构造函数直接使用独立实现；传入 `{&w0, &w1}` 则保留多 Writer 路径，即使列表仅含一个元素也不自动切换。原 `ReadWorker` / `ReadService` 及包含路径保持可用。独立读取器接口见 [`SingleReadWorker`](../src/read/read_layer1/single_writer/read_worker.hpp)，开销对比见[单 Writer 测量](../benchmarks/TUNABLE_SPSC.zh-CN.md)。

current 到时间且有已发布数据就读取，不再等待写指针停止增长；force 模式忽略间隔。`flush()`、换块和 `finish()` 沿用源队列的通知回调，唤醒读线程；多 Writer 路径同时登记来源。事件唤醒后只处理已通知来源，先读这些来源的完成块，再忽略扫描间隔读取其 current；同一来源的重复通知合并，多个来源都保留。扫描到期时遍历全部来源，持续通知不会推迟其他来源的扫描。下游空间通知不指定 Writer，会强制重试全部来源。`flush()` 只发布并请求读取，不等待回调完成；批量发布时未满发布次数的数据仍需 `flush()` 才能立即可见。历史性能数据来自旧读取策略，不能据此推断本次重构的性能。

## 写线程同步回调

回调先定义，再将其具体类型传给 Writer：

```cpp
auto rcvCallback = [&](std::int32_t thread_id, const std::byte* data, std::size_t size) noexcept -> void {
    // 与 01_basic 的回调签名完全一致。
};
using Context = SyncContext<decltype(rcvCallback)>;
auto writer = make_writer<Writer<1, true, std::uint16_t, Context>>();
TwoBufQueue queue(rcvCallback, writer);
const bool written = writer.write(std::uint32_t{123}); // 回调返回后 write 才返回。
writer.finish();
queue.stop();
```

`Writer` 在原模板参数后只增加 `TSyncContext=void`：`void` 使用原有异步管道，非 void 类型直接执行同步回调。单 Writer 和多个 Writer 使用同一类型，多个 Writer 在 TwoBufQueue 构造函数中一次性传入。TwoBufQueue 根据实际 Writer 数量构造唯一共享 `SyncContext<Callback>`：一个 Writer 不加锁，多个 Writer 通过同一个互斥锁串行执行回调。`SyncContext::invoke()` 内判断构造时确定的锁标志；回调仍保留具体类型、可内联，单 Writer 热路径有是否加锁的判断，不执行 mutex 加锁操作。运行期间不能追加 Writer 或改变锁标志。

带序号的用法为 `SeqWriter<TSeq, 1, true, std::uint16_t, Context>`，回调仍为 `void(std::int32_t, TSeq, const std::byte*, std::size_t) noexcept`，与 `05_seq_writer` 一致。`SeqWriter` 同样只在原模板参数后增加 `TSyncContext=void`，不增加同步 bool 参数或单/多 Writer 类型别名；已有异步用法保持不变。

`SyncContext` 保存具体 Lambda / 函数对象类型，Writer 持有类型明确的 Context 对象指针，通过模板成员调用回调，不使用函数指针、`std::function` 或虚函数分发。`write_mem` 直接借用输入内存；`write` 的单参数直接借用对象表示，多参数只在调用期间拼接成原有紧密字节布局。回调只能在本次调用期间使用数据，不得保存指针；`write` 返回成功前回调已完成。无锁或加锁的回调都由调用该 Writer 的生产线程执行，不保证多个 Writer 的全局顺序；多 Writer 回调不要重入同一队列写入，否则会再次取得同一互斥锁。

同步队列不建立输出池、复制线程或回调线程，读取侧 `TwoBufQueueOptions` 不参与同步执行；`started()` 表示回调绑定可用，复制侧 `stats()` 字段为零。Writer 原构造函数仍初始化源池/源块，`flush()` / `finish()` 实现保持不变，`queue.stop()` 在全部 Writer `finish()`、写线程结束后回收空源块。同步消息不占用源块，仍受 `TDataLen` 的长度限制；未绑定或已 finish 的 Writer 写入返回 false。完整例子见 [06_sync_writer.cpp](../examples/two_buf_queue/06_sync_writer.cpp)。
