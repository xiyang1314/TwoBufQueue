// TwoBufQueueBatch：整批回调，而不是逐条回调。
// 适合想一次性拿到一大块连续数据自己切分的场景，比如想多线程去处理一个batch。

#include "../example_support.hpp"
using namespace TwoBufQueue;

int main() try {
    //--------------- 1. 初始化：无长度前缀的 frameless writer ------------------
    auto writer = make_writer<Writer<1, false>>();

    std::size_t values = 0;
    // 消费者回调：整批拿到连续数据，自己切分每条数据。切分也可以参照read_records函数
    auto rcvCallback = [&](const DataBatch& batch) noexcept -> bool {
        for (const auto& frame : batch.frames)
            values += frame.size / sizeof(std::uint64_t); // 每条 8 字节一个值
        return true; // 整批处理完；false 表示整批未处理，下次会再次处理。
    }; // 注意：函数一结束batch内存可能就被释放，所以如果多线程处理一个batch, 必须在回调函数结束前完成各子任务，函数一结束就不能再使用该batch的内存

    ::TwoBufQueue::TwoBufQueueBatch queue(rcvCallback, writer);

    //--------------- 2. 生产：写 100 个 uint64_t -------------------------------
    bool written = true;
    for (std::uint64_t i = 0; i < 100; ++i)
        written &= writer.write(i);

    //--------------- 3. 结束 --------------------------------------------------
    writer.finish();
    queue.stop();

    //--------------- 4. 校验 --------------------------------------------------
    example::require(written && values == 100 &&
                     queue.error_code() == ReadError::none, "batch delivery mismatch");
    std::puts("PASS: 100 frameless records delivered in batches");
} catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
