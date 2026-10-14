#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace TwoBufQueue {

// Very short critical sections only: no allocation/deallocation, callbacks, I/O or thread waits under the lock.
// Nonrecursive. Descheduling the owner can still cause a long wait; there is no wait-time bound.
class SpinLock {
public:
    SpinLock() noexcept = default;
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    // One atomic test-and-set when uncontended; under contention poll reads first instead of repeatedly requesting exclusive cache-line ownership.
    void lock() noexcept {
        if (try_lock()) return;
        std::uint32_t spins = 0;
        for (;;) {
            while (locked_.test(std::memory_order_relaxed)) {
                cpu_relax();
                // Yield under sustained contention to reduce spinning while a lock owner is preempted.
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

    // release publishes critical-section writes to the next owner that successfully acquires the lock.
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

    // atomic_flag is guaranteed lock-free; true means a thread holds the lock.
    std::atomic_flag locked_ = ATOMIC_FLAG_INIT;
};

}  // namespace TwoBufQueue
