#pragma once
#include "TwoBufQueue/src/utils/wait.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace TwoBufQueue {
template<bool TIsNAlloc, bool TIsNUse> struct WriterBlock;
enum class ReadError : std::uint32_t;

// Fast monotonic clock for scan scheduling, not Unix timestamps for logging.
// x86/x64 reuse the project's calibrated TSC with MSVC, GCC or Clang; no Windows API is required.
// Platforms without invariant TSC use std::chrono::steady_clock; callers still use nanosecond durations.
// A distinct time_point type prevents mixing scan times with log timestamps or other clock domains.
class FastSteadyClock {
public:
    using rep = std::int64_t;
    using period = std::nano;
    using duration = std::chrono::nanoseconds;
    using time_point = std::chrono::time_point<FastSteadyClock, duration>;
    static constexpr bool is_steady = true;

    // Reuse logging-time frequency calibration with an independent monotonic anchor for scan scheduling.
    [[nodiscard]] static time_point now() noexcept;
};

// Each valid write advances at least one byte; reject empty writes when there is no prefix and min = 0.
// Reserve one extra slot and validate addition, division and slot-count overflow during configuration.
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
// Raw backpressure-wait counter: TSC ticks on x86/x64, steady_clock ticks elsewhere.
[[nodiscard]] std::uint64_t read_wait_counter() noexcept;

} // namespace TwoBufQueue
