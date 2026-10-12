#pragma once

#include <cstdint>

namespace TwoBufQueue {

struct FastEpochCalibration {
    bool uses_tsc = false;
    std::uint64_t reference_tsc = 0;
    std::int64_t reference_epoch_ns = 0;
    double ticks_per_ns = 1.0;
};

// Initializes the shared TSC frequency and the Unix-epoch/steady-clock anchors.
// Control paths call this before saving so the first completed range pays no calibration.
void initialize_fast_epoch_clock() noexcept;

// Returns Unix epoch nanoseconds. On x86 with an invariant TSC this uses one
// rdtsc plus a calibrated conversion; other targets use system_clock.
[[nodiscard]] std::int64_t fast_epoch_time_ns() noexcept;

// 单调时间，单位纳秒：扫描期限使用此接口，不能使用上面的 Unix 时间戳。
// 与日志时间共用 TSC 频率校准，但以 steady_clock 为独立基准；不支持时回退 steady_clock。
// TSC 快速路径要求硬件计数器恒定且跨 CPU 同步，与现有日志时间的使用约定一致。
[[nodiscard]] std::int64_t fast_steady_time_ns() noexcept;

// Control/test-plane inspection; never needed on the save hot path.
[[nodiscard]] bool fast_epoch_clock_uses_tsc() noexcept;

// Immutable calibration snapshot for raw TSC values stored by LogProducer.
// LogMerge can convert a record without touching the producer's hot path.
[[nodiscard]] FastEpochCalibration fast_epoch_calibration() noexcept;

}  // namespace TwoBufQueue
