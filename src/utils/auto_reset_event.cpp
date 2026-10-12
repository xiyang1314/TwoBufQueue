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
        // 系统原子地比较状态并入睡，通知线程无需拿锁来填补中间的时间空隙。
        // 虚假唤醒、超时或被其他等待者抢走信号后，统一回到上面重新检查。
        wait_on_state(expected, deadline);
    }
    // 不清除信号/通知，避免注销时丢掉刚刚到达的 set。
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
        // 向上取整，避免不足 1ms 时反复做零时长系统调用；不将超长有限等待变成无限。
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
    // 32 位 Linux 使用 64 位 time_t 时，超时结构需要对应的 time64 系统调用。
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
    // Apple 允许低内存等造成提前返回；返回公共循环重新检查条件与同一总期限。
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
    // 等待者尚未进入系统等待或刚刚退出时，ENOENT 属于正常竞争结果。
    if (::os_sync_wake_by_address_any(&state_, sizeof(std::uint32_t),
                                      OS_SYNC_WAKE_BY_ADDRESS_NONE) == -1 && errno != ENOENT)
        std::terminate();
#endif
}

}  // namespace TwoBufQueue
