// Writer / TwoBufQueue 的参数总览：所有模板参数、构造参数和配置字段都显式填写。
// 主线程写两条记录，TwoBufQueue 内部的复制线程与回调线程独立执行。
#include "../example_support.hpp"
#include <chrono>
using namespace TwoBufQueue;
using namespace std::chrono_literals;

int main() try {
    // 1. Writer<publish_write_ptr_count, TIsWriteLen, TDataLen>
    // publish_write_ptr_count = 4：每成功写 4 条发布一次写位置；1 表示逐条发布，必须 > 0。
    // 未满次数时可调用 flush() 发布；finish() / 换块也会发布剩余数据。
    // uint32_t 最大值表示仅在 flush / 换块 / finish 发布，write 不递减发布计数。
    // 可配合下面的 is_scan_current=false 使用，减少写入热路径的开销。
    // TIsWriteLen = true：每条记录带长度前缀；false 为原始字节，需使用 TwoBufQueueBatch。
    // TDataLen = uint32_t：前缀占 4 字节，也是 write_mem 的长度参数类型。
    // 可选无符号整数类型；负载长度须能放进该类型，整条记录（含前缀）须能放进源块。
    constexpr std::uint32_t publish_write_ptr_count = 4;
    constexpr bool is_write_len = true;
    using DataLen = std::uint32_t;
    using MyWriter = Writer<publish_write_ptr_count, is_write_len, DataLen>;

    // 2. WriterSourceConfig 的全部字段：源块池、源队列、来源编号和背压。
    const WriterSourceConfig source{
        .block_size = 4096,       // 每个源块的字节数，包含记录的长度前缀。
        .pool_min = 2,            // 初始预分配的源块数，0 <= pool_min <= pool_max。
        .pool_max = 8,            // 源块总数上限，必须 > 0；不是消息条数。
        .thread_id = 7,           // 非负的来源编号，不是系统线程 ID；多个 Writer 不能重号。
        .queue_capacity = 16,     // 源队列的交接记录容量（含开始记录），至少为 2；独立于池容量。
        .backpressure = WriterBackpressureConfig{
            .mode = BackpressureMode::Block, // 取不到下一块时等待；Drop 表示本条写入立即失败。
            .spin_count = 100,               // Block 模式先 pause 重试这些次，再进入定时重试。
            .wait_get_timeout = 2s,          // 单次等待总时限，含自旋；超时 write 返回 false，须 > 0。
        },
    };

    // 3. 显式构造源池、源队列、Writer；它们的寿命都要覆盖 TwoBufQueue。
    // WriterPool(min_count, max_count, factory, max_free_count)：最后一项限制保留的空闲块数。
    constexpr std::size_t max_free_count = 8;
    WriterPool pool(source.pool_min, source.pool_max,
                    SpscBlockFactory<SpscWriteBlock>{source.block_size}, max_free_count);
    // make_writer_queue(pool, capacity, enable_stats)：true 开启队列申请失败原因统计。
    constexpr bool enable_stats = true;
    auto source_queue = make_writer_queue(pool, source.queue_capacity, enable_stats);
    // Writer(queue, thread_id, backpressure)：三个构造参数全部传入。
    MyWriter writer(&source_queue, source.thread_id, source.backpressure);
    // 不需要手动管理源池/源队列时，可用下面一行替换上面三个对象：
    // auto writer = make_writer<MyWriter>(source); // WriterT 显式传入，source 的字段已全部填写。
    // 此时 Writer 的实际类型为 OwnedWriter<MyWriter>，队列模板的 WriterT 相应改成 decltype(writer)。

    // 4. TwoBufQueueOptions 的全部字段：输出侧资源和读取线程；与 source 的容量分别设置。
    const TwoBufQueueOptions options{
        .output_block_size = 8192,    // 每个输出块的数据字节数，至少能容纳最大的源块可读范围。
        .output_frame_capacity = 16,  // 每输出块最多存几段源数据；一段 frame 可以包含多条记录。
        .output_pool_min = 2,         // 输出池初始块数，0 <= min <= max。
        .output_pool_max = 8,         // 输出池总块数上限，必须 > 0。
        .output_queue_capacity = 8,   // 输出交接队列的槽位数，必须 > 0；与输出池上限独立。
        .read_layer1_thread_count = 1,// 读取线程数；实际限制在 [1, Writer 数量]，Writer 轮询分配。
        .read_config = ReadConfig{
            .idle_spin_count = 100,          // 无新完成块时先 pause 轮询的次数；0 表示直接等待。
            .idle_rescan_interval = 1ms,     // 当前块初始扫描间隔，必须 > 0。
            .worker_cpu = -1,                // -1 不绑核；非负值指定逻辑 CPU，所有读线程使用同一值。
            .idle_rescan_max_interval = 16ms,// 连续两次空扫描时等待翻倍的上限，须 >= 初始间隔。
            .finish_timeout = 2s,            // stop 慢退时收齐源数据的总时限，必须 > 0。
            .work_wait_min_interval = 1ms,   // 普通等待的最小时长；事件可提前唤醒，必须 > 0。
            .finish_retry_interval = 1ms,    // 慢退仍有剩余数据时的重试间隔，必须 > 0。
            .is_scan_current = true,         // false 只读取完成块，尾部在换块或 finish 后交付。
        },
        .cancel_claim_mode = CancelClaimMode::Keep, // 按原入队顺序选择可领取块；QueueTail 为取消后排队尾。
        .output_mode = HandoffMode::MPSC,           // 独立回调线程；多个复制线程可共用输出队列。
        .output_reader_mode = ReaderMode::WaitSignal, // 无数据时等待通知；Polling 为持续轮询。
        .output_reader_count = 1,                  // SPSC/MPSC 必须为 1；SPMC/MPMC 可设 1..64。
        .output_retry_wait = 1ms,                  // WaitSignal 下，回调拒绝后的重试等待。
        .output_enable_stats = true,              // 开启输出交接队列统计。
    };
    // 扫描到数据后恢复初始间隔；扫描时更新时间，与后续回调是否成功无关。
    // flush / 换块 / finish 通知只唤醒对应来源，先读完成块再立即检查 current。
    // is_scan_current=false 时，通知与超时都不检查 current；停队列前须 finish 所有 Writer。
    // flush 不等待读取完成；扫描超时仍检查全部来源，空间通知强制重试全部来源。
    // finish_timeout 不会中断正在执行的业务回调；业务回调应及时返回。
    // cancel_claim_mode 作用于输出 HandoffQueue，不改变 Writer 的源顺序队列。
    // Keep 允许已领取块并行处理，不保证完成顺序；HandoffOptions 中也使用同名字段。
    // 逐条用户回调忽略返回值；正常数据下不触发取消。TwoBufQueueBatch 回调 false 时按此模式重试。

    // 5. TwoBufQueue<RecordCallback, WriterT>：两个模板参数都显式传入。
    // RecordCallback 是回调类型；WriterT 是传入的 Writer 类型（同一队列必须一致）。
    // 普通 Writer 回调的三个参数：来源编号、负载地址、负载字节数（不含长度前缀）。
    // 回调要求 noexcept；此逐条接口忽略返回值。data 只在本次回调内有效。
    auto on_record = [](std::int32_t thread_id, const std::byte* data, std::size_t size) noexcept {
        std::printf("writer=%d: %.*s\n", static_cast<int>(thread_id),
                    static_cast<int>(size), reinterpret_cast<const char*>(data));
    };
    // 单 Writer 构造：TwoBufQueue(callback, writer引用, options)，构造时启动读取线程。
    ::TwoBufQueue::TwoBufQueue<decltype(on_record), MyWriter> queue(on_record, writer, options);
    // 多 Writer 构造只替换第二个参数：queue(on_record, {&writer1, &writer2}, options)。
    // 每个 Writer 使用自己的源池/源队列，由一个生产线程使用，并设置不同的 thread_id。
    // read_layer1_thread_count > 1 可并行搬运多个 Writer；输出回调由独立线程执行。
    // Writer / SeqWriter 的非 void TSyncContext 启用写线程内逐条同步回调，见 06_sync_writer.cpp。
    example::require(queue.started(), "reader failed to start");

    // 6. 只写两条，演示不足 4 条时主动发布；先结束 Writer，再等待队列收尾。
    constexpr char message[] = "hello";
    constexpr DataLen message_size = sizeof(message) - 1; // 不写末尾的 '\0'。
    const bool first = writer.write_mem(message, message_size);
    const bool second = writer.write('o', 'k');
    writer.flush();
    writer.finish();
    queue.stop();
    example::require(first && second && queue.error_code() == ReadError::none, "write/read failed");
    std::puts("PASS: all Writer and TwoBufQueue parameters specified");
} catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
