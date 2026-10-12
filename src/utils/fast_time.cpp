#include "TwoBufQueue/src/utils/fast_time.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#include <emmintrin.h>
#elif (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__)
#include <cpuid.h>
#endif

namespace TwoBufQueue {
namespace {

std::int64_t system_epoch_time_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
std::uint64_t read_tsc() noexcept { return __rdtsc(); }

// 扫描时序先完成此前的读取，再取得 TSC；日志热路径保持原来的读取方式。
std::uint64_t read_ordered_tsc() noexcept {
    _mm_lfence();
    const auto value = __rdtsc();
    _ReadWriteBarrier();
    return value;
}

[[maybe_unused]] bool has_invariant_tsc() noexcept {
    int registers[4]{};
    __cpuid(registers, static_cast<int>(0x80000000u));
    if (static_cast<std::uint32_t>(registers[0]) < 0x80000007u) return false;
    __cpuid(registers, static_cast<int>(0x80000007u));
    return (static_cast<std::uint32_t>(registers[3]) & (1u << 8)) != 0;
}
#elif (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__)
std::uint64_t read_tsc() noexcept {
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    __asm__ __volatile__("rdtsc" : "=a"(low), "=d"(high));
    return (static_cast<std::uint64_t>(high) << 32) | low;
}

// LFENCE 在这里仅用于约束扫描时间的读取顺序，不负责发布日志字节。
std::uint64_t read_ordered_tsc() noexcept {
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    __asm__ __volatile__("lfence\n\trdtsc" : "=a"(low), "=d"(high) :: "memory");
    return (static_cast<std::uint64_t>(high) << 32) | low;
}

[[maybe_unused]] bool has_invariant_tsc() noexcept {
    const unsigned int maximum = __get_cpuid_max(0x80000000u, nullptr);
    if (maximum < 0x80000007u) return false;
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    __cpuid(0x80000007u, eax, ebx, ecx, edx);
    return (edx & (1u << 8)) != 0;
}
#else
[[maybe_unused]] bool has_invariant_tsc() noexcept { return false; }
#endif

std::int64_t system_steady_time_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct FastEpochClock {
    bool use_tsc = false;
    std::uint64_t reference_tsc = 0;
    std::int64_t reference_epoch_ns = 0;
    double ticks_per_ns = 1.0;
    // 扫描时间的独立锚点；频率共用 ticks_per_ns，绝不把 Unix 时间当成扫描期限。
    std::uint64_t reference_steady_tsc = 0;
    std::int64_t reference_steady_ns = 0;

    FastEpochClock() noexcept {
#if defined(FAST_LOG_DISABLE_TSC)
        // 测试/构建选项：强制走标准时钟，验证不支持 TSC 的平台回退路径。
        return;
#endif
#if (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))) || \
    ((defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__))
        if (!has_invariant_tsc()) return;

        const auto steady_begin = std::chrono::steady_clock::now();
        const std::uint64_t tsc_begin = read_tsc();
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        const std::uint64_t tsc_end = read_tsc();
        const auto steady_end = std::chrono::steady_clock::now();
        const double elapsed_ns =
            std::chrono::duration<double, std::nano>(steady_end - steady_begin)
                .count();
        if (tsc_end <= tsc_begin || elapsed_ns <= 0.0) return;
        const double calibrated =
            static_cast<double>(tsc_end - tsc_begin) / elapsed_ns;
        if (!std::isfinite(calibrated) || calibrated < 0.1 ||
            calibrated > 20.0) {
            return;
        }

        const std::uint64_t anchor_before = read_tsc();
        const std::int64_t epoch_ns = system_epoch_time_ns();
        const std::uint64_t anchor_after = read_tsc();
        reference_tsc =
            anchor_before + ((anchor_after - anchor_before) / 2u);
        reference_epoch_ns = epoch_ns;
        ticks_per_ns = calibrated;
        // 保留原有日志时间锚点，再取得扫描专用的 steady_clock 锚点。
        const auto steady_before = read_ordered_tsc();
        reference_steady_ns = system_steady_time_ns();
        const auto steady_after = read_ordered_tsc();
        reference_steady_tsc = steady_before + (steady_after - steady_before) / 2u;
        use_tsc = true;
#endif
    }

    [[nodiscard]] std::int64_t now_ns() const noexcept {
#if (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))) || \
    ((defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__))
        if (use_tsc) {
            const std::uint64_t current = read_tsc();
            if (current >= reference_tsc) {
                const double elapsed_ns =
                    static_cast<double>(current - reference_tsc) / ticks_per_ns;
                return reference_epoch_ns +
                       static_cast<std::int64_t>(elapsed_ns);
            }
        }
#endif
        return system_epoch_time_ns();
    }
};

const FastEpochClock& fast_clock() noexcept {
    static const FastEpochClock clock;
    return clock;
}

}  // namespace

void initialize_fast_epoch_clock() noexcept { (void)fast_clock(); }

std::int64_t fast_epoch_time_ns() noexcept { return fast_clock().now_ns(); }

std::int64_t fast_steady_time_ns() noexcept {
    // 共用日志时间的频率校准；扫描只使用独立的 steady_clock 锚点。
    const FastEpochClock& clock = fast_clock();
#if (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))) || \
    ((defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__))
    if (clock.use_tsc) {
        const auto current = read_ordered_tsc();
        if (current >= clock.reference_steady_tsc) {
            const auto elapsed = static_cast<double>(current - clock.reference_steady_tsc) /
                                 clock.ticks_per_ns;
            return clock.reference_steady_ns + static_cast<std::int64_t>(elapsed);
        }
    }
#else
    (void)clock;
#endif
    // 频率校准失败、不支持恒定 TSC，或计数器早于锚点时，使用标准单调时钟。
    return system_steady_time_ns();
}

bool fast_epoch_clock_uses_tsc() noexcept { return fast_clock().use_tsc; }

FastEpochCalibration fast_epoch_calibration() noexcept {
    const FastEpochClock& clock = fast_clock();
    return {
        clock.use_tsc,
        clock.reference_tsc,
        clock.reference_epoch_ns,
        clock.ticks_per_ns,
    };
}

}  // namespace TwoBufQueue
