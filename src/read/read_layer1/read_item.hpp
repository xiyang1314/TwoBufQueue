#pragma once
#include "TwoBufQueue/src/read/read_layer1/types.hpp"
#include "TwoBufQueue/src/utils/brqHelp.hpp"
#include "TwoBufQueue/src/utils/wait.hpp"

namespace TwoBufQueue {
// Per-source scan interval and previous-empty state; Service owns queue operations, callbacks, cursors and backpressure statistics.
class ReadItem {
public:
    using SteadyClock = FastSteadyClock;
    using TimePoint = SteadyClock::time_point;
    explicit ReadItem(const ReadConfig& config) noexcept
        : initial_read_interval_(config.idle_rescan_interval),
          read_interval_(config.idle_rescan_interval),
          max_read_interval_(config.idle_rescan_max_interval) {}

    [[nodiscard]] bool should_read_current(TimePoint now, bool force,
                                           std::size_t read_size) noexcept {
        if (!force && now < next_read_time_) return false; // A not-yet-due check is not a scan.
        const bool empty = read_size == 0;
        if (empty && last_scan_empty_) {
            if (read_interval_ > max_read_interval_ / 2) read_interval_ = max_read_interval_;
            else read_interval_ *= 2;
        } else {
            read_interval_ = initial_read_interval_;
        }
        last_scan_empty_ = empty;
        update_next_read_time(now); // Update timing at the scan itself, independently of later callback results.
        return !empty;
    }
    [[nodiscard]] TimePoint next_read_time() const noexcept { return next_read_time_; }
private:
    void update_next_read_time(TimePoint now) noexcept {
        // utils/wait.hpp computes now + delay, saturating at the clock limit to avoid integer overflow.
        next_read_time_ = deadline_after(now, read_interval_);
    }

    const std::chrono::nanoseconds initial_read_interval_;
    std::chrono::nanoseconds read_interval_;
    const std::chrono::nanoseconds max_read_interval_;
    TimePoint next_read_time_{};
    bool last_scan_empty_ = false;
};
} // namespace TwoBufQueue
