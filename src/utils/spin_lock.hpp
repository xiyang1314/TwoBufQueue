#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace TwoBufQueue {

// 仅用于很短的临界区；锁内不得分配/释放内存、调用回调、执行 I/O 或等待线程。
// 不可重入。持锁线程被调度出去时仍可能久等，不提供等待时间上界。
class SpinLock {
public:
    SpinLock() noexcept = default;
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    // 无竞争只做一次原子置位；竞争时先只读轮询，避免反复争抢缓存行写权限。
    void lock() noexcept {
        if (try_lock()) return;
        std::uint32_t spins = 0;
        for (;;) {
            while (locked_.test(std::memory_order_relaxed)) {
                cpu_relax();
                // 竞争持续时主动让出执行机会，降低持锁线程被抢占后的空转压力。
                if (++spins == 256) {
                    std::this_thread::yield();
                    spins = 0;
                }
            }
            if (try_lock()) return;
        }
    }

    [[nodiscard]] bool try_lock() noexcept {
        return !locked_.test_and_set(std::memory_order_acquire);
    }

    // release 将临界区写入发布给下一个 acquire 成功的持锁线程。
    void unlock() noexcept { locked_.clear(std::memory_order_release); }

private:
    static void cpu_relax() noexcept {
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
        _mm_pause();
#elif defined(_MSC_VER) && (defined(_M_ARM) || defined(_M_ARM64))
        __yield();
#elif defined(__i386__) || defined(__x86_64__)
        __builtin_ia32_pause();
#elif defined(__aarch64__) || (defined(__arm__) && __ARM_ARCH >= 7)
        __asm__ __volatile__("yield");
#else
        std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
    }

    // atomic_flag 保证无锁；true 表示已有线程持有本锁。
    std::atomic_flag locked_ = ATOMIC_FLAG_INIT;
};

}  // namespace TwoBufQueue
