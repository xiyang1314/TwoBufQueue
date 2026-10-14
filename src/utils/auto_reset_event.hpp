#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>

namespace TwoBufQueue {

struct AutoResetEventTestAccess;

// Auto-reset event: retain at most one signal; successful waiting consumes it atomically. Repeated set calls do not accumulate.
// Supports multiple notifiers/waiters without ordering guarantees; try_wait may consume a signal first.
// set/reset/try_wait take no mutex; set enters the OS only when waiters need waking.
// OS wakeups involve scheduling; the whole set operation is not guaranteed wait-free in the real-time sense.
// Windows 8+, Linux and macOS 14.4+; wait implementations are in auto_reset_event.cpp.
// Finish every call before destruction, including set calls still executing an OS wakeup.
class AutoResetEvent {
public:
    using Clock = std::chrono::steady_clock;

    explicit AutoResetEvent(bool signaled = false) noexcept
        : state_(signaled ? signal_bit : 0) {}
    AutoResetEvent(const AutoResetEvent&) = delete;
    AutoResetEvent& operator=(const AutoResetEvent&) = delete;

    void set() noexcept {
        // One RMW publishes the signal, leaves a recheck notification and reads the waiter registration count.
        // Notification is independent of the signal: try_wait/reset must not erase a waiter's need to recheck stop conditions.
        const auto previous = state_.fetch_or(signal_bit | wake_bit, std::memory_order_release);
        if ((previous & waiter_mask) == 0) return;
        // Notify whenever waiters are registered, even if the old notification bit remains set, to avoid missed wakeups among multiple waiters.
        // Do not wait for a business mutex; the OS compare-and-sleep operation closes the check-to-sleep gap.
        wake_one();
    }

    // Discard only an unconsumed signal, preserving waiter count and pending notification. Cannot undo a successful wait.
    // Normally used in coordinated states such as before startup; concurrent set follows atomic modification order.
    void reset() noexcept {
        state_.fetch_and(~signal_bit, std::memory_order_relaxed);
    }

    // Without a signal, perform one read; with a signal, CAS ensures exactly one caller consumes it.
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

    // true consumes a signal; false means the deadline passed without consumption. An existing signal takes priority over an expired deadline.
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

    // true consumes a signal; false means stop or timeout. Observing stop does not consume the signal.
    // stop_request must outlive the call; store(true, release) before set() for prompt wakeup.
    // Do not poll the stop variable; prompt cancellation covers one blocked waiter. One set is not a broadcast cancellation for all waiters.
    [[nodiscard]] bool wait_until(Clock::time_point deadline,
                                 const std::atomic<bool>& stop_request) noexcept {
        if (stop_request.load(std::memory_order_acquire)) return false;
        if (try_wait()) return true;
        if (deadline <= Clock::now()) return false;
        return wait_slow(&deadline, &stop_request);
    }

    // Relative-duration overload: return immediately for stopped/signalled/nonpositive-duration cases, without entering an OS wait.
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

    // Multiple waiters may register together; check overflow before CAS rather than using a mutex.
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

    // Clear the recheck notification before testing stop/signal; a later set raises it again.
    // Sleep in the OS only when the signal and pending-notification bits are clear and the waiter count is unchanged.
    std::uint32_t prepare_wait() noexcept {
        return state_.fetch_and(~wake_bit, std::memory_order_acquire) & waiter_mask;
    }

    bool wait_slow(const Clock::time_point* deadline,
                   const std::atomic<bool>* stop_request) noexcept;
    void wait_on_state(std::uint32_t expected, const Clock::time_point* deadline) noexcept;
    void wake_one() noexcept;

    // Lowest bit: signal; next bit: recheck notification; remaining bits: registered waiters. Every modification is RMW.
    // set before registration is observed by the post-registration acquire check.
    // set after wait preparation changes at least wake_bit, so the OS comparison fails instead of sleeping.
    // Even if try_wait consumes or reset clears the signal, wake_bit survives until a waiter rechecks.
    alignas(4) std::atomic<std::uint32_t> state_{0};
};

}  // namespace TwoBufQueue
