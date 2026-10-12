#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>

namespace TwoBufQueue {

struct AutoResetEventTestAccess;

// 自动复位事件：最多保留一个信号，成功等待原子地消费；重复 set 不累计。
// 支持多个通知者/等待者，不保证等待顺序，try_wait 可以抢先消费信号。
// set/reset/try_wait 不取互斥锁；set 仅在需要唤醒等待者时调用系统接口。
// 系统唤醒有调度开销，不承诺整个 set 是实时系统意义下的 wait-free 操作。
// 平台：Windows 8+、Linux、macOS 14.4+；等待实现位于 auto_reset_event.cpp。
// 析构前须确保所有调用结束，包括尚在系统唤醒途中的 set。
class AutoResetEvent {
public:
    using Clock = std::chrono::steady_clock;

    explicit AutoResetEvent(bool signaled = false) noexcept
        : state_(signaled ? signal_bit : 0) {}
    AutoResetEvent(const AutoResetEvent&) = delete;
    AutoResetEvent& operator=(const AutoResetEvent&) = delete;

    void set() noexcept {
        // 一次 RMW 同时发布信号、留下重新检查通知，并取得登记人数。
        // 通知位独立于信号：try_wait/reset 不能抹掉等待者应重新检查停止条件的事实。
        const auto previous = state_.fetch_or(signal_bit | wake_bit, std::memory_order_release);
        if ((previous & waiter_mask) == 0) return;
        // 只要有人登记就通知；不能因旧通知位仍在而跳过，否则多个等待者之间可能漏唤醒。
        // 不等待业务互斥锁；系统的“比较并入睡”操作负责封闭检查与入睡之间的空隙。
        wake_one();
    }

    // 只丢弃未消费的信号，保留等待人数及待检查通知。不能撤销已成功的等待。
    // 通常只在启动前等已协调好的场景调用；与 set 并发时以原子修改顺序为准。
    void reset() noexcept {
        state_.fetch_and(~signal_bit, std::memory_order_relaxed);
    }

    // 无信号时只有一次读取，有信号时用 CAS 保证只被一个调用者消费。
    [[nodiscard]] bool try_wait() noexcept {
        auto observed = state_.load(std::memory_order_relaxed);
        while ((observed & signal_bit) != 0) {
            if (state_.compare_exchange_weak(observed, observed & ~signal_bit,
                                             std::memory_order_acquire,
                                             std::memory_order_relaxed)) return true;
        }
        return false;
    }

    void wait() noexcept {
        if (try_wait()) return;
        (void)wait_slow(nullptr, nullptr);
    }

    // true 表示消费信号；false 表示到期且未消费信号。已有信号优先于过期期限。
    [[nodiscard]] bool wait_until(Clock::time_point deadline) noexcept {
        if (try_wait()) return true;
        if (deadline <= Clock::now()) return false;
        return wait_slow(&deadline, nullptr);
    }

    [[nodiscard]] bool wait_for(Clock::duration timeout) noexcept {
        if (try_wait()) return true;
        if (timeout <= Clock::duration::zero()) return false;
        const auto now = Clock::now();
        const auto deadline = now >= Clock::time_point::max() - timeout
            ? Clock::time_point::max() : now + timeout;
        return wait_slow(&deadline, nullptr);
    }

    // true 表示消费信号；false 表示停止或超时。观察到停止时不消费信号。
    // stop_request 必须活到调用结束；先 store(true, release)，再 set() 才能及时唤醒。
    // 不轮询停止变量；及时取消适用于单个阻塞等待者，一次 set 不是多等待者取消广播。
    [[nodiscard]] bool wait_until(Clock::time_point deadline,
                                 const std::atomic<bool>& stop_request) noexcept {
        if (stop_request.load(std::memory_order_acquire)) return false;
        if (try_wait()) return true;
        if (deadline <= Clock::now()) return false;
        return wait_slow(&deadline, &stop_request);
    }

    // 相对时长版本；已停止/已有信号/非正时长均直接返回，不进入系统等待。
    [[nodiscard]] bool wait_for(Clock::duration timeout,
                               const std::atomic<bool>& stop_request) noexcept {
        if (stop_request.load(std::memory_order_acquire)) return false;
        if (try_wait()) return true;
        if (timeout <= Clock::duration::zero()) return false;
        const auto now = Clock::now();
        const auto deadline = now >= Clock::time_point::max() - timeout
            ? Clock::time_point::max() : now + timeout;
        return wait_slow(&deadline, &stop_request);
    }

private:
    friend struct AutoResetEventTestAccess;
    static constexpr std::uint32_t signal_bit = 1;
    static constexpr std::uint32_t wake_bit = 2;
    static constexpr std::uint32_t waiter_unit = 4;
    static constexpr std::uint32_t waiter_mask = ~(signal_bit | wake_bit);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "AutoResetEvent requires lock-free 32-bit atomics");
    static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t),
                  "The native wait word must contain exactly 32 bits");

    // 多个等待者可同时登记；用 CAS 在修改前检查溢出，不借助互斥锁。
    void register_waiter() noexcept {
        auto observed = state_.load(std::memory_order_relaxed);
        for (;;) {
            if ((observed & waiter_mask) == waiter_mask) std::terminate();
            if (state_.compare_exchange_weak(observed, observed + waiter_unit,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) return;
        }
    }

    void unregister_waiter() noexcept {
        state_.fetch_sub(waiter_unit, std::memory_order_relaxed);
    }

    // 必须先清待检查通知，再检查停止/信号；之后到达的 set 会重新置位。
    // 系统只允许在“没有信号、没有待检查通知、人数未变”时入睡。
    std::uint32_t prepare_wait() noexcept {
        return state_.fetch_and(~wake_bit, std::memory_order_acquire) & waiter_mask;
    }

    bool wait_slow(const Clock::time_point* deadline,
                   const std::atomic<bool>* stop_request) noexcept;
    void wait_on_state(std::uint32_t expected, const Clock::time_point* deadline) noexcept;
    void wake_one() noexcept;

    // 最低位为信号，次低位为待检查通知，其余位是登记人数；所有修改都使用 RMW。
    // set 先于登记：登记后的 acquire 检查可观察到它。
    // set 晚于准备等待：至少 wake_bit 改变，系统比较失败就不会入睡。
    // 即使信号被 try_wait 抢走或被 reset 清除，wake_bit 仍保留到等待者再次检查。
    alignas(4) std::atomic<std::uint32_t> state_{0};
};

}  // namespace TwoBufQueue
