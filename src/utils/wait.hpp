#pragma once
#include <chrono>
#include <type_traits>

namespace TwoBufQueue {
// Bound values before unit conversion so huge millisecond durations cannot overflow when implicitly converted to nanoseconds.
// Native clock durations pass through; other precisions saturate within the clock's representable range.
template<class Clock, class Rep, class Period>
[[nodiscard]] constexpr typename Clock::duration wait_duration(
    std::chrono::duration<Rep, Period> timeout) noexcept {
    using Duration = typename Clock::duration;
    if constexpr (std::is_same_v<decltype(timeout), Duration>) {
        return timeout > Duration::zero() ? timeout : Duration::zero();
    } else {
        const auto ticks = std::chrono::duration<long double, typename Duration::period>(timeout).count();
        if (!(ticks > 0)) return Duration::zero();
        if (ticks >= static_cast<long double>((Duration::max)().count())) return (Duration::max)();
        return Duration{static_cast<typename Duration::rep>(ticks)};
    }
}

// Non-positive delays are already due; positive delays saturate at the clock limit.
// Compare against max-delay, not max-now: the latter can overflow for negative origins.
template <class Clock>
[[nodiscard]] constexpr typename Clock::time_point deadline_after(
    std::chrono::time_point<Clock> now, typename Clock::duration delay) noexcept {
    using Point = typename Clock::time_point;
    if (delay <= Clock::duration::zero()) return now;
    return now >= Point::max() - delay ? Point::max() : now + delay;
}

} // namespace TwoBufQueue
