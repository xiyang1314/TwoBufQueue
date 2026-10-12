// "第一层"回调：直接读取 writer 的源数据块，不经过第二层（拷贝成输出块 + 逐条回调）。
// 消费端要实现 ReadWriterDataCallback 的 6 个固定方法，读取线程会回调它们。

#include "../example_support.hpp"
#include <cstring>
using namespace TwoBufQueue;

// 消费者：实现第一层读取所需的 6 个回调方法，直接处理源块里的字节。
struct Collector {
    std::size_t count = 0;
    long long total = 0;

    // 以下 6 个方法是 ReadService 要求的固定接口（见 read_writer_data_callback.hpp）。
    void begin_read_complete(std::int64_t) noexcept {}     // 一批"已完成块"开始前
    bool read_complete(SpscWriteBlock* block) noexcept {   // 一个已写满/已结束的块
        const auto bytes = block->readable();              // 整块数据范围
        return parse(block, bytes.data(), bytes.size());
    }
    void end_read_complete() noexcept {}
    void begin_read_current(std::int64_t) noexcept {}      // 正在写入的当前块开始前
    bool read_current(SpscWriteBlock* block, const std::byte* ptr, std::size_t size) noexcept {
        return parse(block, ptr, size);                    // 当前块新增的数据
    }
    void end_read() noexcept {}

private:
    // 把 [ptr, ptr+size) 按 [长度前缀][负载] 逐条解析（规则与 example_support 一致）。
    bool parse(SpscWriteBlock* block, const std::byte* ptr, std::size_t size) noexcept {
        return example::messages(block, ptr, size,
            [&](std::int32_t, const std::byte* data, std::size_t n) noexcept {
                if (n != sizeof(int)) return false;
                int value = 0;
                std::memcpy(&value, data, sizeof(value));
                total += value;
                ++count;
                return true;
            });
    }
};

int main() try {
    //--------------- 1. 初始化：一个 writer + 一个"第一层"读取器 ----------------
    auto writer = make_writer<Writer<>>();                 // 默认 Writer：带长度前缀
    Collector collector;
    ReadWorker reader({}, collector);                      // {} 用默认 ReadConfig
    if (!reader.add_writer(&writer)) throw std::runtime_error("add_writer failed");
    if (!reader.start()) throw std::runtime_error("start failed");

    //--------------- 2. 生产者写 100 条 int -----------------------------------
    bool written = true;
    for (int i = 1; i <= 100; ++i)
        written &= writer.write(i);

    //--------------- 3. 结束：先 finish 生产者，再停读取线程（收尾当前块）-------
    writer.finish();                                       // 结束写入并交接最后一块
    reader.stop();                                         // 等剩余数据读完再停

    //--------------- 4. 校验 --------------------------------------------------
    example::require(written && reader.error_code() == ReadError::none, "write/read failed");
    example::require(collector.count == 100 && collector.total == 5050, "delivery mismatch");
    std::puts("PASS: 100 records read directly at layer 1 (no layer-2 copy)");
} catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
