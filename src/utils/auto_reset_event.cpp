#include "TwoBufQueue/src/utils/auto_reset_event.hpp"

#include <cerrno>
#include <limits>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#elif defined(__APPLE__)
#if !defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__) || \
    __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ < 140400
#error "AutoResetEvent requires a macOS deployment target of 14.4 or newer"
#endif
#include <os/os_sync_wait_on_address.h>
#else
#error "AutoResetEvent native waits support Windows, Linux and macOS"
#endif

namespace TwoBufQueue {

bool AutoResetEvent::wait_slow(const Clock::time_point* deadline,
                               const std::atomic<bool>* stop_request) noexcept {
    register_waiter();
    bool consumed = false;
    for (;;) {
        const auto expected = prepare_wait();
        if (stop_request && stop_request->load(std::memory_order_acquire)) break;
        if ((consumed = try_wait())) break;
        if (deadline && *deadline <= Clock::now()) break;
        // The OS compares state and sleeps atomically; notifying threads need no lock to close the check-to-sleep gap.
        // Spurious wakeups, timeouts or another waiter consuming the signal all return to the common recheck loop above.
        wait_on_state(expected, deadline);
    }
    // Keep signals/notifications intact so unregistering cannot lose a newly arriving set.
    unregister_waiter();
    return consumed;
}

void AutoResetEvent::wait_on_state(std::uint32_t expected,
                                   const Clock::time_point* deadline) noexcept {
    Clock::duration remaining{};
    if (deadline) {
        const auto now = Clock::now();
        if (*deadline <= now) return;
        remaining = *deadline - now;
    }
#if defined(_WIN32)
    DWORD timeout = INFINITE;
    if (deadline) {
        // Round upward to avoid repeated zero-duration system calls below 1 ms; never turn a long finite wait into infinity.
        const auto ms = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
        timeout = ms >= INFINITE ? INFINITE - 1 : static_cast<DWORD>(ms);
    }
    if (!::WaitOnAddress(&state_, &expected, sizeof(expected), timeout) &&
        ::GetLastError() != ERROR_TIMEOUT) std::terminate();
#elif defined(__linux__)
    timespec timeout{};
    if (deadline) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(remaining);
        if (seconds.count() > std::numeric_limits<time_t>::max()) {
            timeout.tv_sec = std::numeric_limits<time_t>::max();
        } else {
            timeout.tv_sec = static_cast<time_t>(seconds.count());
            timeout.tv_nsec = static_cast<long>(
                std::chrono::ceil<std::chrono::nanoseconds>(remaining - seconds).count());
        }
    }
    // A 32-bit Linux build with 64-bit time_t needs the corresponding time64 syscall for its timeout structure.
#if defined(SYS_futex_time64)
    constexpr auto wait_syscall = sizeof(time_t) > sizeof(long) ? SYS_futex_time64 : SYS_futex;
#else
    constexpr auto wait_syscall = SYS_futex;
#endif
    const auto result = ::syscall(wait_syscall, &state_, FUTEX_WAIT_PRIVATE, expected,
                                  deadline ? &timeout : nullptr, nullptr, 0);
    if (result == -1 && errno != EAGAIN && errno != EINTR && errno != ETIMEDOUT)
        std::terminate();
#elif defined(__APPLE__)
    const int result = deadline
        ? ::os_sync_wait_on_address_with_timeout(
              &state_, expected, sizeof(expected), OS_SYNC_WAIT_ON_ADDRESS_NONE,
              OS_CLOCK_MACH_ABSOLUTE_TIME,
              static_cast<std::uint64_t>(std::chrono::ceil<std::chrono::nanoseconds>(remaining).count()))
        : ::os_sync_wait_on_address(&state_, expected, sizeof(expected), OS_SYNC_WAIT_ON_ADDRESS_NONE);
    // Apple may return early under memory pressure and other conditions; recheck conditions under the same total deadline.
    if (result == -1 && errno != EINTR && errno != ETIMEDOUT && errno != ENOMEM && errno != EFAULT)
        std::terminate();
#endif
}

void AutoResetEvent::wake_one() noexcept {
#if defined(_WIN32)
    ::WakeByAddressSingle(&state_);
#elif defined(__linux__)
    if (::syscall(SYS_futex, &state_, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0) == -1)
        std::terminate();
#elif defined(__APPLE__)
    // ENOENT is a normal race when a waiter has not entered or has just left the system wait.
    if (::os_sync_wake_by_address_any(&state_, sizeof(std::uint32_t),
                                      OS_SYNC_WAKE_BY_ADDRESS_NONE) == -1 && errno != ENOENT)
        std::terminate();
#endif
}

}  // namespace TwoBufQueue
