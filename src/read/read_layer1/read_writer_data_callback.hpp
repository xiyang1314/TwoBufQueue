#pragma once
#include "TwoBufQueue/src/data/spsc_write_block.hpp"
#include <cstddef>
#include <cstdint>

namespace TwoBufQueue {
// 回调接口样例，不是基类。用户提供同名方法后直接用 ReadService<MyCallback>。
// true：当前传入范围已全部处理；false：未处理，本轮 read_once 立即停止读取。
// 不得保存源指针、修改游标或归还 Block；不能处理部分数据后返回 false。
struct ReadWriterDataCallback {
    void begin_read_complete(std::int64_t /*time_ns*/) noexcept {}
    bool read_complete(SpscWriteBlock* /*block*/) noexcept { return true; }
    void end_read_complete() noexcept {}
    void begin_read_current(std::int64_t /*time_ns*/) noexcept {}
    bool read_current(SpscWriteBlock* /*block*/, const std::byte* /*read_ptr*/,
                      std::size_t /*read_size*/) noexcept { return true; }
    // 每次 read_once 都调用；失败后也调用，不能将此通知理解为全部成功。
    void end_read() noexcept {}
};
} // namespace TwoBufQueue
