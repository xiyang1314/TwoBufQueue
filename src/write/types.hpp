#pragma once

#include "TwoBufQueue/src/data/spsc_write_block.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <source_location>

namespace TwoBufQueue {

// Session 的写入不会隐式换块；空间不足由外层调用 get_next_session 处理。
enum class SessionWriteResult : std::uint8_t {
    success,
    insufficient_space,
    invalid_argument,
    exceeds_block_capacity, // 整条记录（含长度/序号前缀）超过 block 容量。
    error,
};


// 内存池耗尽时的处理方式；只在换块冷路径生效。
enum class BackpressureMode : std::uint8_t {
    Drop,   // 立即丢弃当前记录，让生产线程继续运行。
    Block,  // 等待保存线程归还块；超过 wait_get_timeout 后本条写入失败。
};

// Writer 持有的背压配置；只在申请下一块失败时执行。
struct WriterBackpressureConfig {
    // 池耗尽时采用丢弃还是阻塞策略。
    BackpressureMode mode = BackpressureMode::Drop;
    // Block 模式进入定时重试前执行的 pause + get 重试次数。
    std::uint32_t spin_count = 1'000;
    // 单次阻塞取块的总时限，包含自旋和休眠；必须为正且适配时钟范围，由 Writer 构造函数校验。
    std::chrono::milliseconds wait_get_timeout = std::chrono::seconds{5};
};

// 内存写入器的背压统计快照；所有计数只在压力相关的非正常路径更新。
struct WriterBackpressureStats {
    // 首次取不到下一块、开始处理背压的次数。
    std::uint64_t pressure_events = 0;
    // 因池耗尽而没有写入的记录总数。
    std::uint64_t dropped_records = 0;
    // 真正进入 Block 等待流程的次数。
    std::uint64_t blocked_events = 0;
    // 所有 Block 等待时长的累计计数；x86/x64 上单位是 TSC tick。
    std::uint64_t blocked_ticks = 0;
    // 单次 Block 等待的最长计数；单位与 blocked_ticks 相同。
    std::uint64_t max_blocked_ticks = 0;
};

}  // namespace TwoBufQueue
