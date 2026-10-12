//同步执行回调函数，也就是没有读线程，在写线程调用回调函数。方便调试用
//其他代码和异步都一样，唯一不同的是创建writer的时候需要传入回调函数类型：auto writer = make_writer<Writer<1, true, std::uint16_t, Context>>()
//如果是多个writer，也就是多线程,这里会自动给回调函数加锁
#include "../example_support.hpp"
#include <cstring>
using namespace TwoBufQueue;

int main() try {
    struct CustomData { int Data; };

    //---------------1.初始化-----------------------------------------------
    std::size_t count = 0;
    const auto writer_thread = std::this_thread::get_id();
    bool same_thread = true;

    // 消费者回调函数
    auto rcvCallback = [&](std::int32_t thread_id, const std::byte* data, std::size_t size) noexcept -> void {
        same_thread &= thread_id == 0 && std::this_thread::get_id() == writer_thread;
        if (++count <= 3) {
            std::printf("message: len = %.*s\n", static_cast<int>(size), reinterpret_cast<const char*>(data));
        } else {//无固定类型参数，根据自定义规则解析
            int type = 0;
            std::memcpy(&type, data, sizeof(type));
            std::printf("message: len = %d, type = %d\n", static_cast<int>(size), type);
        }
    };

    // 非 void Context 表示同步；回调类型可内联，TwoBufQueue 根据 Writer 数量决定是否加锁。
    using Context = SyncContext<decltype(rcvCallback)>;
    auto writer = make_writer<Writer<1, true, std::uint16_t, Context>>(); //----->就这一句Context参数不同
    ::TwoBufQueue::TwoBufQueue queue(rcvCallback, writer); // 构造时绑定回调，不启动读取线程

    //---------------2.生产者写数据------------------------------------------
    bool written = true;
    for (std::string_view text : {"hello", "TwoBufQueue", "goodbye"})
        written &= writer.write_mem(text.data(), static_cast<std::uint16_t>(text.size()));

    //无固定类型参数，收到数据后在消费者回调函数里自己根据规则解析
    written &= writer.write(1, 1, static_cast<short>(2));
    written &= writer.write(2, CustomData{3});

    //---------------3.结束-------------------------------------------------
    writer.finish(); // 生产者结束（由用户负责），沿用原有实现
    queue.stop();    // 消息已在 write 中处理完，这里回收空源块

    //---------------4.校验测试结果------------------------------------------
    example::require(written && count == 5 && same_thread, "write/drain or message count mismatch");
    std::puts("PASS: 5 messages, callback on writer thread");
} catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
