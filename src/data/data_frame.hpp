#pragma once
#include <cstddef>
#include <cstdint>
namespace TwoBufQueue {
// 同步保存的输入视图；调用返回前数据须保持可读，不拥有源内存。
struct DataView {
    std::int32_t thread_id = -1;
    const std::byte* data = nullptr;
    std::size_t size = 0;
};

// ReadBlock 内一个源范围的视图，可能包含多条记录，不持有 Slice。
// thread_id 是注册 Writer 时的来源编号，不是操作系统线程号。
struct DataFrame {
    std::int32_t thread_id = -1;
    const std::byte* data = nullptr;
    std::size_t size = 0;
    std::int64_t read_time_ns = 0;
};

}
