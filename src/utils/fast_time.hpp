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

// Monotonic nanoseconds for scan deadlines; never use the Unix timestamp interface above.
// Share TSC frequency calibration with logging, using an independent steady_clock reference; fall back to steady_clock if unsupported.
// The TSC fast path requires an invariant, cross-CPU-synchronized counter, matching the existing logging-time contract.
[[nodiscard]] std::int64_t fast_steady_time_ns() noexcept;

// Control/test-plane inspection; never needed on the save hot path.
[[nodiscard]] bool fast_epoch_clock_uses_tsc() noexcept;

// Immutable calibration snapshot for raw TSC values stored by LogProducer.
// LogMerge can convert a record without touching the producer's hot path.
[[nodiscard]] FastEpochCalibration fast_epoch_calibration() noexcept;

}  // namespace TwoBufQueue
