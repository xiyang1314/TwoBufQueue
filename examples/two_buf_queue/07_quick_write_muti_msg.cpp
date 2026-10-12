#include "../example_support.hpp"
#include <array>
#include <chrono>
#include <cstring>
using namespace TwoBufQueue;

int main() try {
    //---------------1.初始化-----------------------------------------------
    using HotWriter = Writer<UINT32_MAX>; // 按块发布，保留长度前缀；Session 不要求额外模板参数。
    auto writer = make_writer<HotWriter>({.block_size = 4096, .pool_min = 8, .pool_max = 8});
    std::size_t count = 0;
    auto rcvCallback = [&](std::int32_t thread_id, const std::byte* data, std::size_t size) noexcept {
        (void)thread_id;
        std::array<std::uint64_t, 4> value{};
        if (size != sizeof(value)) std::terminate();
        std::memcpy(value.data(), data, size);
        if (value != std::array<std::uint64_t,4>{count,count+1,count+2,count+3}) std::terminate();
        ++count;
    };
    ::TwoBufQueue::TwoBufQueue queue(rcvCallback, writer);

    //---------------2.生产者写数据------------------------------------------
    std::uint64_t sent = 0;
    auto session = writer.get_current_session(std::chrono::seconds{1}); // 获取失败则错误处理并返回。
    example::require(static_cast<bool>(session), "cannot acquire session within timeout");
    while (sent != 10000) { // 外层：由用户明确选择下一块。
        while (sent != 10000) { // 内层：只写这个块；空间不足就退出。
            const auto result = session.write(std::array<std::uint64_t,4>{sent,sent+1,sent+2,sent+3});
            if (result == SessionWriteResult::insufficient_space) break;
            example::require(result == SessionWriteResult::success, "record exceeds block capacity or write error");
            ++sent; // 只有成功才推进消息位置。
        }
        if (sent == 10000) break; // 全部写完，不再申请下一块。
        session = {}; // 结束旧 Session，仅同步游标；不发布、不换块。
        session = writer.get_next_session(std::chrono::seconds{1});
        example::require(static_cast<bool>(session), "cannot acquire session within timeout");
    }
    session.flush(); // 用户选择调用；只发布位置，不换块，也不结束 Session。
    session = {}; // 操作 Writer.finish 前先结束 Session；也可以用作用域析构。

    //---------------3.结束-------------------------------------------------
    // 关闭 current 扫描时，flush 不交接尾块；finish 在结束时交接最后一块。
    writer.finish();
    queue.stop();
    //---------------4.校验测试结果------------------------------------------
    example::require(count == 10000, "message count mismatch");
    std::puts("PASS: 10000 messages with current/next single-block sessions");
} catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
