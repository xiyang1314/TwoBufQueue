#include "example_support.hpp"
#include <cstring>
using namespace TwoBufQueue;

int main() try {
    struct CustomData { int Data; };

    //---------------1.初始化-----------------------------------------------
    std::size_t count = 0; 
    auto writer = make_writer<Writer<>>();// 生产者的对象writer

    // 消费者回调函数
    auto rcvCallback = [&](std::int32_t thread_id, const std::byte* data, std::size_t size) noexcept -> void {
        (void)thread_id; // 单 writer，thread_id 恒为 0
        if (++count <= 3) {
            std::printf("message: len = %.*s\n", static_cast<int>(size), reinterpret_cast<const char*>(data));
        } else {//无固定类型参数，根据自定义规则解析
            int type = 0;
            std::memcpy(&type, data, sizeof(type));
            std::printf("message: len = %d, type = %d\n", static_cast<int>(size), type);
        }
    };

    ::TwoBufQueue::TwoBufQueue queue(rcvCallback, writer); //传递数据的queue
    
    //---------------2.生产者写数据------------------------------------------
    bool written = true;
    for (std::string_view text : {"hello", "TwoBufQueue", "goodbye"})
        written &= writer.write_mem(text.data(), static_cast<std::uint16_t>(text.size()));
 
    //无固定类型参数，收到数据后在消费者回调函数里自己根据规则解析
    written &= writer.write(1, 1, static_cast<short>(2));     
    written &= writer.write(2, CustomData{3});        

    //---------------3.结束-------------------------------------------------
    writer.finish(); // 生产者结束（由用户负责）
    queue.stop();    // 等待全部记录消费完并停止线程

    //---------------4.校验测试结果------------------------------------------
    example::require(written && count == 5, "write/drain or message count mismatch");
    std::puts("PASS: 5 messages");
} catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
