#pragma once
#include <chrono>
#include <type_traits>

namespace TwoBufQueue {
// 在单位转换前限制范围，避免超大毫秒值隐式转换到纳秒时整数溢出。
// 原生时钟 duration 直接使用；其他精度的等待时长在时钟范围内饱和。
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
