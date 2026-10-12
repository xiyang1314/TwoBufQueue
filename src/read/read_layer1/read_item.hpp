#pragma once
#include "TwoBufQueue/src/read/read_layer1/types.hpp"
#include "TwoBufQueue/src/utils/brqHelp.hpp"
#include "TwoBufQueue/src/utils/wait.hpp"

namespace TwoBufQueue {
// 每个来源保存扫描间隔和上次空扫描状态；队列操作、回调、游标推进和背压统计均在 Service。
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
        if (!force && now < next_read_time_) return false; // 未到期不算一次扫描。
        const bool empty = read_size == 0;
        if (empty && last_scan_empty_) {
            if (read_interval_ > max_read_interval_ / 2) read_interval_ = max_read_interval_;
            else read_interval_ *= 2;
        } else {
            read_interval_ = initial_read_interval_;
        }
        last_scan_empty_ = empty;
        update_next_read_time(now); // 扫描时就更新时间，不依赖后续回调结果。
        return !empty;
    }
    [[nodiscard]] TimePoint next_read_time() const noexcept { return next_read_time_; }
private:
    void update_next_read_time(TimePoint now) noexcept {
        // utils/wait.hpp：计算 now + delay，并在时钟上限处饱和，避免整数溢出。
        next_read_time_ = deadline_after(now, read_interval_);
    }

    const std::chrono::nanoseconds initial_read_interval_;
    std::chrono::nanoseconds read_interval_;
    const std::chrono::nanoseconds max_read_interval_;
    TimePoint next_read_time_{};
    bool last_scan_empty_ = false;
};
} // namespace TwoBufQueue
