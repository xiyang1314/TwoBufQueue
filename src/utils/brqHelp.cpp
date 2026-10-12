#include "TwoBufQueue/src/utils/brqHelp.hpp"
#include "TwoBufQueue/src/utils/fast_time.hpp"
#include "TwoBufQueue/src/base_components/block_slice/writer_block.hpp"
#include "TwoBufQueue/src/read/read_layer1/types.hpp"

#include <atomic>
#include <limits>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace TwoBufQueue {

FastSteadyClock::time_point FastSteadyClock::now() noexcept {
    // FastSteadyClock 只提供 chrono 接口；TSC 读取、校准和回退统一由 fast_time 管理。
    return time_point{duration{fast_steady_time_ns()}};
}

const char* error_message(ReadError code) noexcept {
    switch (code) {
        case ReadError::none: return "";
        case ReadError::null_dependency: return "ReadWorker received null writer, queue or read callback";
        case ReadError::invalid_registration: return "ReadWorker invalid or running add_writer";
        case ReadError::duplicate_writer: return "ReadWorker duplicate writer, queue or thread_id";
        case ReadError::foreign_current_range: return "ReadWorker current range is not owned by the expected queue";
        case ReadError::registration_allocation_failed: return "ReadWorker add_writer allocation failed";
        case ReadError::invalid_start_state: return "ReadWorker invalid configuration or state";
        case ReadError::thread_creation_failed: return "ReadWorker failed to create worker thread";
        case ReadError::cpu_affinity_failed: return "ReadWorker failed to set worker_cpu affinity";
        case ReadError::foreign_queued_range: return "ReadWorker queue contains foreign range";
        case ReadError::range_release_failed: return "ReadWorker failed to release completed block";
        case ReadError::free_range: return "ReadWorker attempted to save free range";
        case ReadError::invalid_range_cursors: return "ReadWorker observed invalid range cursors";
        case ReadError::collection_capacity_exceeded: return "ReadWorker read collection capacity exceeded";
        case ReadError::invalid_read_callback_result: return "ReadWorker invalid read callback result";
        case ReadError::finish_timeout: return "ReadWorker finish timed out; source data may remain unsaved";
    }
    return "ReadWorker unknown error";
}

// 判断候选指针是否位于指定块的闭区间内；块尾指针允许作为空区间终点。
bool pointer_in_range(
    const WriterBlock<>* range, const std::byte* pointer,
    std::uint64_t block_size) noexcept {
    return range != nullptr && pointer >= range->buf_ &&
           pointer <= range->buf_ + block_size;
}

// 按配置把当前读取线程绑定到指定 CPU；负数表示不设置亲和性。
bool pin_current_thread(std::int32_t cpu) noexcept {
    if (cpu < 0) return true;
#ifdef _WIN32
    constexpr std::int32_t cpu_mask_bits =
        static_cast<std::int32_t>(sizeof(DWORD_PTR) * 8);
    if (cpu >= cpu_mask_bits) return false;
    return ::SetThreadAffinityMask(::GetCurrentThread(),
                                   DWORD_PTR{1} << cpu) != 0;
#elif defined(__linux__)
    if (cpu >= CPU_SETSIZE) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
#else
    (void)cpu;
    return false;
#endif
}

// 在短暂空闲轮询中提示处理器降低流水线和超线程资源压力。
void cpu_relax() noexcept {
#ifdef _WIN32
    YieldProcessor();
#elif defined(__i386__) || defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

std::uint64_t read_wait_counter() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    return __rdtsc();
#elif defined(__i386__) || defined(__x86_64__)
    std::uint32_t low = 0, high = 0;
    __asm__ __volatile__("rdtsc" : "=a"(low), "=d"(high));
    return (static_cast<std::uint64_t>(high) << 32) | low;
#else
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

}  // namespace TwoBufQueue
