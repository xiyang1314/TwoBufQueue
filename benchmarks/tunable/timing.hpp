#pragma once
#include "config.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <vector>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#if !defined(_MSC_VER)
#include <cpuid.h>
#endif
#endif

namespace TwoBufQueue::performance {
struct Quantiles { double p50 = 0, p99 = 0, p999 = 0, maximum = 0; };
inline Quantiles quantiles(std::vector<double> values) {
    if (values.empty()) return {};
    std::sort(values.begin(),values.end());
    const auto at = [&](double p) { return values[static_cast<std::size_t>(std::ceil(p * values.size())) - 1]; };
    return {at(0.50),at(0.99),at(0.999),values.back()};
}
class ProbeClock {
public:
    struct Stamp { std::uint64_t ticks; unsigned cpu = 0; };
    explicit ProbeClock(const PerformanceConfig& config) {
        if (config.timer == TimerKind::automatic) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
            initialize_fast_epoch_clock();
            const auto calibration = fast_epoch_calibration();
            unsigned features = 0;
#if defined(_MSC_VER)
            int regs[4]{}; __cpuid(regs,static_cast<int>(0x80000001)); features = static_cast<unsigned>(regs[3]);
#else
            unsigned a = 0,b = 0,c = 0;
            (void)__get_cpuid(0x80000001,&a,&b,&c,&features);
#endif
            if (calibration.uses_tsc && (features & (1u << 27))) {
                kind_ = Kind::tsc; ns_per_tick_ = 1.0 / calibration.ticks_per_ns;
                quantum_ns_ = ns_per_tick_; label_ = "serialized_invariant_tsc";
            }
#elif defined(__APPLE__)
            mach_timebase_info_data_t base{};
            if (mach_timebase_info(&base) == KERN_SUCCESS && base.denom) {
                kind_ = Kind::mach; ns_per_tick_ = static_cast<double>(base.numer) / base.denom;
                quantum_ns_ = ns_per_tick_; label_ = "mach_absolute_time";
            }
#endif
        }
        std::vector<double> empty;
        empty.reserve(config.timer_calibration_samples);
        for (std::uint32_t i = 0; i < config.timer_calibration_samples; ++i) {
            const auto a = read(); const auto b = read();
            if (a.cpu == b.cpu && b.ticks >= a.ticks) empty.push_back(elapsed(a,b));
        }
        overhead_ = quantiles(std::move(empty));
    }
    [[nodiscard]] Stamp read() const noexcept {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        Stamp value{};
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        if (kind_ == Kind::tsc) {
            _mm_lfence(); value.ticks = __rdtscp(&value.cpu); _mm_lfence();
        } else
#elif defined(__APPLE__)
        if (kind_ == Kind::mach) value.ticks = mach_absolute_time(); else
#endif
        value.ticks = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        std::atomic_signal_fence(std::memory_order_seq_cst);
        return value;
    }
    [[nodiscard]] double elapsed(Stamp a, Stamp b) const noexcept { return static_cast<double>(b.ticks-a.ticks) * ns_per_tick_; }
    [[nodiscard]] const char* label() const noexcept { return label_; }
    [[nodiscard]] double quantum_ns() const noexcept { return quantum_ns_; }
    [[nodiscard]] Quantiles overhead() const noexcept { return overhead_; }
private:
    enum class Kind { steady, tsc, mach };
    Kind kind_ = Kind::steady;
    double ns_per_tick_ = 1e9 * static_cast<double>(std::chrono::steady_clock::period::num) /
        static_cast<double>(std::chrono::steady_clock::period::den);
    double quantum_ns_ = ns_per_tick_;
    const char* label_ = "steady_clock";
    Quantiles overhead_{};
};
} // namespace TwoBufQueue::performance
