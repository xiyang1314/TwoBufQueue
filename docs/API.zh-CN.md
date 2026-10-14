# 接口使用指南

先创建生产者使用的 `writer`，再把“接收消息的回调函数”和 writer 交给 `TwoBufQueue`。生产者直接调用 writer 发送；默认情况下，队列在后台复制数据并执行回调。

[中文首页](../README.zh-CN.md) · [English API](API.zh-CN.md) · [接入工程](INTEGRATION.zh-CN.md) · [可运行示例](../examples/README.zh-CN.md)

## 一、先选择需要的用法

| 你的需求 | 使用方式 | 示例 |
|---|---|---|
| 逐条发送，后台逐条接收 | `Writer<>` + `TwoBufQueue` | [基础示例](../examples/01_basic.cpp) |
| 连续快速发送多条消息 | `Writer<UINT32_MAX>` + `WriteSession` | [循环发送](../examples/two_buf_queue/07_quick_write_muti_msg.cpp) |
| 每条消息带一个序号 | `SeqWriter` | [序号示例](../examples/two_buf_queue/05_seq_writer.cpp) |
| 一次回调接收一组数据 | `TwoBufQueueBatch` | [分组接收回调](../examples/two_buf_queue/02_batch_callback.cpp) |
| 回调就在发送线程执行 | 非 void `SyncContext` | [同步回调](../examples/two_buf_queue/06_sync_writer.cpp) |
| 直接读取第一层，省去第二层复制 | 自行组合 `ReadWorker` 与源回调 | [直接读取](../examples/two_buf_queue/04_layer1_callback.cpp) |

第一次使用先看下面的完整例子；只有需要调参数时，再读配置部分。

## 二、一个完整的发送与接收例子

```cpp
#include <TwoBufQueue.hpp>
#include <cstdio>

int main() {
    // 创建发送入口；同时创建它需要的第一层内存池。
    auto writer = TwoBufQueue::make_writer<TwoBufQueue::Writer<>>();

    // 接收消息的回调函数。data 和 size 是本条消息的内容与长度。
    auto on_record = [](std::int32_t source_id, const std::byte* data,
                        std::size_t size) noexcept {
        std::printf("source=%d: %.*s\n", static_cast<int>(source_id),
                    static_cast<int>(size), reinterpret_cast<const char*>(data));
    };

    // 构造时启动后台处理；writer 必须比 queue 活得更久。
    TwoBufQueue::TwoBufQueue queue(on_record, writer);
    const bool ok = writer.write_mem("hello", 5);

    writer.finish(); // 结束发送，交接最后一块。
    queue.stop();    // 等待后台处理收尾。
    return ok && queue.error_code() == TwoBufQueue::ReadError::none ? 0 : 1;
}
```

`write_mem` 返回成功表示消息已提交，不表示回调已经执行。上例默认异步；构造配置错误或分配失败可能抛异常，实际应用可在初始化处捕获处理。

## 三、怎么发送消息

| 函数 | 适合发送什么 | 返回值 |
|---|---|---|
| `writer.write(value...)` | 整数、简单结构体等可平凡复制的值 | `true` 成功；`false` 本次没有写入 |
| `writer.write_mem(data, len)` | 字符串、字节数组、动态内存中的内容 | `true` 成功；`false` 本次没有写入 |

`write` 复制参数的字节内容，多个参数紧密拼接成一条消息。它不接受指针或原生数组作为负载；要发送指针指向的内容，使用 `write_mem`。不能直接写 `std::string` 这类拥有动态内存的对象，应传它的 `data()` 和长度。

```cpp
const std::uint64_t value = 42;
const bool value_ok = writer.write(value);
const bool text_ok = writer.write_mem("hello", 5);
// 检查每次返回值；失败的消息没有进入队列。
```

异步发送会复制输入内容；调用返回后可以复用输入内存。输入地址必须有效，且不能与写入目标重叠。

默认 `Writer<>` 为每条消息保存两字节长度，单条负载最多 65,535 字节；完整消息还必须装入一个第一层块。需要更宽的长度字段时，可使用 `Writer<1, true, std::uint32_t>`。传入 `write_mem` 的长度必须能放进所选类型，不能靠强制转换截断长度。当前不自动拆分超出块容量的消息。

带长度的普通 Writer 支持空消息；不带长度时拒绝空负载。**Session 的 `write_mem` 另外要求长度大于 0。** 数据格式是本进程中的字节表示，不是跨机器序列化协议。

## 四、接收回调的三个参数

```cpp
auto on_record = [](std::int32_t source_id, const std::byte* data,
                    std::size_t size) noexcept {
    // 在这里处理一条消息。
};
```

| 参数 | 含义 |
|---|---|
| `source_id` | 你给 Writer 设置的来源编号；不是操作系统线程 ID |
| `data` | 本条消息的负载地址，不含长度前缀 |
| `size` | 本条消息的负载字节数 |

默认由一个后台消费线程依次调用回调。启用多个消费线程后，回调可能并发执行，应保护共享业务状态。

**数据指针只在这次回调期间有效。** 需要交给业务线程池时，先复制到应用自己持有的存储，再分发任务。不要把 `data` 或 `DataBatch` 的借用指针直接留给异步任务。队列当前不直接接收外部 executor 参数。

收到结构体字节时，先确认长度，再用 `std::memcpy` 复制到本地对象；地址可能没有按结构体对齐。逐条回调返回 `void`，没有“返回 false 要求重试”的机制。`noexcept` 回调在内部处理或记录错误，不向外抛异常。

## 五、flush、交接当前块、finish、stop 的区别

| 操作 | 做什么 | 之后还能继续写吗 |
|---|---|---|
| `writer.flush()` | 发布已经写入的位置，并请求后台读取；不结束当前块 | 可以 |
| `writer.finish_block()` | 交接当前块；下次写入再取新块 | 可以 |
| `writer.finish()` | 结束这个 Writer，并交接最后一块 | 不可以 |
| `queue.stop()` | 收尾复制与消费工作；需要所有生产者已经结束 | 不应再写 |

`flush()` 不等于等待回调完成，也不等于文件落盘。当前块扫描开启时，后台可以读取已发布的部分；关闭扫描时，flush 也不会交付尚未交接的当前块，需调用 `finish_block()` 或最终 `finish()`。

正在使用 Session 时，不直接调用 writer 的这些操作。需要发布时使用 `session.flush()`；需要交接或结束时先释放 Session。

## 六、两层缓存分别怎么配置

第一层属于每个 Writer；第二层属于整个队列。两层容量分别设置，不能把第一层大小当成第二层大小。

```cpp
auto writer = TwoBufQueue::make_writer<TwoBufQueue::Writer<>>({
    .block_size = 4096, // 第一层每块 4 KiB。
    .pool_min = 4,
    .pool_max = 4,      // 第一层数据池上限约 16 KiB。
});
TwoBufQueue::TwoBufQueueOptions options;
options.output_block_size = 64 * 1024;
options.output_pool_min = 2;
options.output_pool_max = 16; // 第二层数据池上限约 1 MiB。
// TwoBufQueue::TwoBufQueue queue(on_record, writer, options);
```

这里的容量计算只包含数据块，不含队列槽位、块元数据和应用输入数据。第二层单块必须能容纳第一层的最大可读范围，通常设为不小于第一层单块。

### 6.1 按需分配与空闲块回收

`pool_min` 是初始块数，`pool_max` 是总块数上限。池里没有可复用块时，按需要创建新块；不必提前分配到最大数量。

底层 `WarmPool` 还支持 `max_free_count`，限制保留的空闲块数。归还时，若空闲块达到此上限且总块数高于 `min_count`，多余块进入回收流程；已借出的块不会被回收。

当前 `make_writer` 和 `TwoBufQueueOptions` 的快捷配置默认保留空闲块，没有提供空闲保留上限字段。需要源池自动回收时，手动构造 `WriterPool(min_count, max_count, factory, max_free_count)`，再连接源队列和 Writer。见[参数示例](../examples/two_buf_queue/08_all_parameters.cpp)。

### 6.2 第一层：WriterSourceConfig

| 参数 | 默认值 | 用户需要理解的含义 |
|---|---|---|
| `block_size` | 64 KiB | 一个第一层块的字节数；含消息前缀 |
| `pool_min` / `pool_max` | 2 / 8 | 初始块数／最大块数；每个 Writer 独立 |
| `thread_id` | 0 | 来源编号；多个 Writer 不能重复 |
| `queue_capacity` | 256 | 块交接记录的容量，至少为 2；不是消息条数 |
| `backpressure.mode` | `Drop` | 取不到新块时，立即失败还是等待 |
| `backpressure.spin_count` | 1,000 | `Block` 等待前先做多少次短暂重试 |
| `backpressure.wait_get_timeout` | 5 秒 | `Block` 的一次等待上限；含短暂重试时间 |

`Drop` 是拒绝本次消息，应用从返回值决定是否重试或丢弃；`Block` 等到超时也会失败。可用 `writer.backpressure_stats()` 查看压力次数、拒绝条数和等待次数。多个第一层池的总容量要按 Writer 数量累计。

### 6.3 第二层与后台线程：TwoBufQueueOptions

| 参数 | 默认值 | 用户需要理解的含义 |
|---|---|---|
| `output_block_size` | 64 KiB | 一个第二层块的数据容量 |
| `output_frame_capacity` | 64 | 每块容纳多少段来源数据；一段可能含多条消息 |
| `output_pool_min` / `output_pool_max` | 2 / 8 | 第二层初始块数／最大块数 |
| `output_queue_capacity` | 8 | 输出块交接容量，与池块数分别设置 |
| `read_layer1_thread_count` | 1 | 复制线程数，最多为 Writer 数量 |
| `output_mode` | `MPSC` | 复制线程与消费线程的并发组合 |
| `output_reader_count` | 1 | 消费线程数；多个线程需 `SPMC` / `MPMC` |
| `output_reader_mode` | `WaitSignal` | 空闲时等通知；`Polling` 为持续轮询 |
| `output_retry_wait` | 1 ms | 批量回调拒绝后，在等待模式下的重试间隔 |
| `cancel_claim_mode` | `QueueTail` | 批量回调拒绝后的重新领取策略 |
| `output_enable_stats` | `false` | 是否统计输出交接队列的活动 |

块数上限必须大于 0，初始块数不超过上限；线程配置与输出模式要匹配。全部显式参数和约束见[参数示例](../examples/two_buf_queue/08_all_parameters.cpp)。

## 七、什么时候读取尚未写满的当前块

当前块就是生产者还在使用、尚未交接的第一层块。用 `options.read_config.is_scan_current` 决定后台是否读取它。

| 设置 | 行为 | 适合的需求 |
|---|---|---|
| `true`（默认） | 读取当前块里已经发布的消息，也读取完成块 | 消息较稀疏，希望写完后即使生产者空闲也能被读取 |
| `false` | 只读取已经交接的完成块 | 可以等一批到达，重视生产者提交性能 |

Writer 的第一个模板参数控制多久发布一次位置：`Writer<1>` 每条发布；`Writer<10>` 每十条发布；`Writer<UINT32_MAX>` 只在 flush、换块或结束时发布。

**发布位置不等于唤醒线程。** `flush()`、换块与 `finish()` 请求读取；仅写入一个已发布位置不代表消费者马上运行。生产者空闲前若还有不足发布批次的数据，需主动 flush；关闭扫描时则需交接当前块。

常用读取参数都在 `options.read_config` 中：

| 参数 | 默认值 | 作用 |
|---|---|---|
| `idle_rescan_interval` | 5 ms | 当前块初始扫描间隔 |
| `idle_rescan_max_interval` | 30 ms | 连续空扫描后间隔逐步增大的上限 |
| `idle_spin_count` | 1,000 | 无完成块时等待前先做的短暂轮询次数 |
| `work_wait_min_interval` | 1 ms | 普通等待的最小时长；事件可以提前唤醒 |
| `worker_cpu` | -1 | 默认由系统调度；指定核心是否可用取决于平台 |
| `finish_timeout` | 5 秒 | 收尾复制的总时限；超时需检查错误 |
| `finish_retry_interval` | 1 ms | 收尾未完成时再次尝试的间隔 |

扫描到数据后恢复初始间隔，连续空扫描则逐步延长。调小间隔不保证操作系统按相同精度调度线程。`finish_timeout` 也不会强行中断正在执行的业务回调。

## 八、连续发送：WriteSession

Session 适合一次循环写很多条。它只写当前这一块，空间不足就返回，由应用的外层循环决定换块。

使用异步 `Writer<UINT32_MAX>` 或对应的 SeqWriter；其他发布次数和同步 Writer 不提供这个接口。

| 接口 | 含义 |
|---|---|
| `get_current_session(timeout)` | 复用当前块；没有块时尝试获取 |
| `get_next_session(timeout)` | 交接当前块，然后获取下一块 |
| `session.write(...)` / `write_mem(...)` | 在这个块里写一条消息；不会自动换块 |
| `session.flush()` | 发布已写数据并通知；不换块，也不结束 Session |
| `session = {}` 或离开作用域 | 结束使用，把位置同步回 writer；不会自动 flush |

超时参数默认 0，只尝试一次；正值表示在时限内尝试获取。返回空 Session 时，用 `if (!session)` 处理失败，不能继续写。

内层循环只在成功后增加消息下标。`insufficient_space` 时退出内层，先释放旧 Session，再由外层获取下一块。完整双循环见[快速写多条记录](../examples/two_buf_queue/07_quick_write_muti_msg.cpp)。

| SessionWriteResult | 含义 | 应用怎么处理 |
|---|---|---|
| `success` | 这一条写入成功 | 推进消息下标 |
| `insufficient_space` | 当前块剩余空间不足 | 退出内层，由外层换块 |
| `invalid_argument` | `write_mem` 长度为 0 | 修正参数；输入地址仍须由调用方保证有效 |
| `exceeds_block_capacity` | 整条消息超过块的总容量 | 调整消息或配置；换新块也装不下 |
| `error` | Session 无效等其他错误 | 停止本次写入并处理错误 |

一个 Writer 同时只用一个 Session。Session 的寿命短于 writer，使用期间不直接调用 writer 操作；不需要 `close()`。最后是否 `session.flush()` 由应用选择，结束生产前必须先释放 Session。

## 九、多生产者与多消费者

每个生产线程使用自己的 Writer，并设置不同来源编号；同一队列中的 Writer 类型要一致。全部 Writer 在队列构造时传入，不能运行到一半再追加。

```cpp
auto writer0 = TwoBufQueue::make_writer<TwoBufQueue::Writer<>>({.thread_id = 0});
auto writer1 = TwoBufQueue::make_writer<TwoBufQueue::Writer<>>({.thread_id = 1});
TwoBufQueue::TwoBufQueue queue(on_record, {&writer0, &writer1});
// 线程 0 只操作 writer0；线程 1 只操作 writer1。
```

一个 Writer 只允许一个生产线程写。多个 Writer 不保证全局先后顺序；增加消费线程后，也不保证业务回调完成顺序。多消费者是分担工作，不是每个消费者都收到所有消息。

| 输出模式 | 复制线程（输出生产者） | 消费工作线程 |
|---|---|---|
| `SPSC` | 1 | 1 |
| `MPSC` | 一个或多个 | 1 |
| `SPMC` | 1 | 一个或多个 |
| `MPMC` | 一个或多个 | 一个或多个 |

复制线程数用 `read_layer1_thread_count`，消费线程数用 `output_reader_count`。多消费者的共享状态应自行同步。[多生产者示例](../examples/two_buf_queue/03_multi_thread_writer.cpp) · [完整线程配置](../examples/two_buf_queue/08_all_parameters.cpp)。

## 十、同步回调、序号与批量回调

### 10.1 回调在生产者线程执行

先定义回调，把它的类型放进 `SyncContext`，再传给 Writer 的最后一个模板参数：

```cpp
auto on_record = [](std::int32_t, const std::byte*, std::size_t) noexcept {
    // 接收消息的回调函数；此处就在发送线程执行。
};
using Context = TwoBufQueue::SyncContext<decltype(on_record)>;
auto writer = TwoBufQueue::make_writer<
    TwoBufQueue::Writer<1, true, std::uint16_t, Context>>();
TwoBufQueue::TwoBufQueue queue(on_record, writer);
const bool ok = writer.write(std::uint64_t{42}); // 返回前回调已执行。
writer.finish();
queue.stop();
```

默认 `TSyncContext=void` 为异步；非 void 为同步。同步模式不建立第二层池或后台读取线程，单 Writer 不加互斥锁，多 Writer 的回调用共享互斥锁串行执行。多个 Writer 的同步回调不要重入同一队列。慢回调会直接拖累生产者。

不要用普通 Writer 的 `output_mode=Sync` 代替这个写法；该组合会报配置错误。`TwoBufQueueBatch` 的 `Sync` 则表示在复制线程执行批量回调，含义不同。

### 10.2 消息需要序号

`SeqWriter` 的回调多一个序号参数：`void(source_id, sequence, data, size) noexcept`。序号分配不保证多个 Writer 的全局回调顺序。同步写法同样在最后传 Context。[序号示例](../examples/two_buf_queue/05_seq_writer.cpp)。

### 10.3 直接处理一批数据

`TwoBufQueueBatch` 把 `DataBatch` 交给回调。一段 `DataFrame` 可以包含多条记录，不是一条消息。返回 `true` 接受这一批，返回 `false` 留给后续重试；无论返回什么，都不能在回调外保存借用指针。

`Writer<..., false>` 不保存消息长度，需由业务自己识别边界，并使用批量接口。直接读第一层的源回调也是借用视图：返回 `true` 后由读取器推进位置并归还完成块，用户不自行消费或归还。[批量示例](../examples/two_buf_queue/02_batch_callback.cpp) · [直接读取示例](../examples/two_buf_queue/04_layer1_callback.cpp)。

## 十一、正确结束程序

1. 停止生产；如果有生产者线程，先 join。
2. 释放所有仍在使用的 Session。
3. 对每个 Writer 调用 `finish()`。
4. 调用 `queue.stop()`，再检查 `queue.error_code()`。
5. 若回调向业务线程池派发了自有数据任务，再等待这些任务结束。

writer、回调以及回调访问的业务资源须覆盖相应处理的寿命。收尾超时或错误不能当成已全部处理；文件 flush、关闭和落盘确认由应用自行处理。[异步日志示例](ASYNC_LOGGING.zh-CN.md) 演示了完整收尾。

## 十二、需要自己组合底层组件时

常规使用通过 `make_writer` 管理第一层资源。自定义创建或统计时，可显式建立 `WriterPool`、`make_writer_queue(pool, capacity, enable_stats)` 和 `Writer(queue, id, backpressure)`；这些对象都须覆盖队列运行期。

每个源队列只能有一个读取器负责，单 Writer 与多 Writer 有各自的读取实现。源范围、输出 frame 与消息是不同概念；确认、重试和归还交给对应读取器，避免重复归还。参数与底层例子见[示例索引](../examples/README.zh-CN.md)及[全部参数](../examples/two_buf_queue/08_all_parameters.cpp)。
