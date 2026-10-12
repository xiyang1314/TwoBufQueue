#pragma once
#include "TwoBufQueue/src/utils/wait.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace TwoBufQueue {
template<bool is_n_alloc, bool is_n_use> struct WriterBlock;
enum class ReadError : std::uint32_t;

// 扫描调度使用的快速单调时钟，不是日志的 Unix 时间戳。
// x86/x64 使用项目现有的 TSC 校准，支持 MSVC、GCC、Clang；无需 Windows API。
// 不支持恒定 TSC 的平台使用 std::chrono::steady_clock，调用方仍统一使用纳秒时长。
// 使用独立的 time_point 类型，防止把扫描时间与日志时间戳或其他时钟的时间点混用。
class FastSteadyClock {
public:
    using rep = std::int64_t;
    using period = std::nano;
    using duration = std::chrono::nanoseconds;
    using time_point = std::chrono::time_point<FastSteadyClock, duration>;
    static constexpr bool is_steady = true;

    // 复用日志时间的频率校准，以独立的单调时间锚点计算扫描时间。
    [[nodiscard]] static time_point now() noexcept;
};

// 每个有效写入至少推进一个字节；无前缀且 min=0 时拒绝空写入。
// 预留一个额外槽位，并在配置阶段检查加法、除法及槽位数量溢出。
[[nodiscard]] constexpr std::size_t source_slice_capacity(
    std::uint64_t block_size, std::size_t min_message_length,
    std::size_t prefix_size = sizeof(std::uint16_t)) {
    if (prefix_size != 0 && prefix_size != 1 && prefix_size != 2 &&
        prefix_size != 4 && prefix_size != 8)
        throw std::invalid_argument("invalid message prefix size");
    if (prefix_size && min_message_length >
        ((std::numeric_limits<std::uint64_t>::max)() >> ((8 - prefix_size) * 8)))
        throw std::invalid_argument("minimum message does not fit its length prefix");
    if (block_size > (std::numeric_limits<std::size_t>::max)() ||
        prefix_size > block_size || min_message_length > block_size - prefix_size)
        throw std::invalid_argument("source block cannot hold a minimum message");
    const auto record_size = prefix_size + min_message_length;
    const auto count = block_size / (record_size ? record_size : 1);
    if (block_size == 0 || count == (std::numeric_limits<std::size_t>::max)())
        throw std::invalid_argument("source slice capacity overflows or is empty");
    return static_cast<std::size_t>(count + 1);
}

// Common stateless utilities; declarations avoid including write/read implementation headers.
[[nodiscard]] const char* error_message(ReadError code) noexcept;
[[nodiscard]] bool pointer_in_range(const WriterBlock<false, true>* range, const std::byte* pointer,
                                    std::uint64_t block_size) noexcept;
[[nodiscard]] bool pin_current_thread(std::int32_t cpu) noexcept;
void cpu_relax() noexcept;
// 背压等待统计使用的原始计数：x86/x64 为 TSC tick，其他平台为 steady_clock tick。
[[nodiscard]] std::uint64_t read_wait_counter() noexcept;

} // namespace TwoBufQueue
