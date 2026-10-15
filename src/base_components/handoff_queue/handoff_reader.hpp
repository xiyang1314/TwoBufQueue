#pragma once

#include "handoff_types.hpp"
#include "TwoBufQueue/src/utils/auto_reset_event.hpp"
#include <bit>
#include <thread>
#include <type_traits>
#include <utility>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace TwoBufQueue {

// Independent consumer: Queue supplies Resource, try_claim/finish/cancel_claim/has_ready.
// Does not depend on concrete HandoffQueue, WarmPool or ClaimQueue types.
template<class TQueue, class TCallback>
class HandoffReader {
    static_assert(std::is_nothrow_invocable_r_v<bool, TCallback&, typename TQueue::Resource&>,
                  "callback must return bool and be noexcept");
public:
    HandoffReader(TQueue& queue, HandoffOptions options, TCallback callback)
        : queue_(queue), options_(validate_handoff_options(options)), callback_(std::move(callback)) {
        if (options_.mode != HandoffMode::Sync) {
            workers_ = new Worker[options_.reader_count];
            if (!start()) {
                delete[] workers_;
                throw std::runtime_error("HandoffReader could not start threads");
            }
        }
    }
    HandoffReader(const HandoffReader&) = delete;
    HandoffReader& operator=(const HandoffReader&) = delete;
    ~HandoffReader() noexcept {
        stop();
        if (is_processing()) std::terminate();
        delete[] workers_;
    }

    // Notify after enqueue succeeds. Sync mode consumes in the caller; asynchronous mode only wakes waiters.
    void notify() noexcept {
        if (options_.mode == HandoffMode::Sync) process_pending();
        else notify_worker();
    }
    void process_pending() noexcept {
        if (options_.mode != HandoffMode::Sync) return;
        requests_.fetch_add(1, std::memory_order_acq_rel);
        if (processing_.exchange(true, std::memory_order_acq_rel)) return;
        for (;;) {
            const auto observed = requests_.load(std::memory_order_acquire);
            for (;;) {
                const auto result = read_once();
                if (result == ReadResult::Finished) continue;
                if (result == ReadResult::Cancelled) {
                    processing_.store(false, std::memory_order_release);
                    return;
                }
                break;
            }
            // acquire observes requests updates published by busy callers' exchange(true).
            (void)processing_.exchange(false, std::memory_order_acq_rel);
            // Close the send/notify race between the last empty check and exit; rejection still ends this round immediately.
            if (requests_.load(std::memory_order_acquire) == observed) return;
            if (processing_.exchange(true, std::memory_order_acq_rel)) return;
        }
    }
    [[nodiscard]] bool is_processing() const noexcept { return processing_.load(std::memory_order_acquire); }
    [[nodiscard]] bool is_running() const noexcept { return running_.load(std::memory_order_acquire); }

    // The caller serializes lifecycle operations; never call start/stop or destroy this object from its own callback.
    bool start() noexcept {
        if (!workers_) return true;
        if (is_running()) return false;
        stopping_.store(false, std::memory_order_release);
        try {
            for (std::size_t i = 0; i < options_.reader_count; ++i)
                workers_[i].thread = std::thread([this, i] { run(i); });
        } catch (...) {
            stop();
            return false;
        }
        running_.store(true, std::memory_order_release);
        return true;
    }
    void stop() noexcept {
        if (!workers_) return;
        stopping_.store(true, std::memory_order_release);
        for (std::size_t i = 0; i < options_.reader_count; ++i) workers_[i].event.set();
        for (std::size_t i = 0; i < options_.reader_count; ++i) {
            auto& thread = workers_[i].thread;
            if (thread.joinable()) {
                if (thread.get_id() == std::this_thread::get_id()) std::terminate();
                thread.join();
            }
        }
        running_.store(false, std::memory_order_release);
    }

private:
    enum class ReadResult { Empty, Finished, Cancelled };
    struct alignas(64) Worker { AutoResetEvent event; std::thread thread; };
    ReadResult read_once() noexcept {
        auto claim = queue_.try_claim();
        if (!claim) return ReadResult::Empty;
        if (callback_(*claim)) {
            if (!queue_.finish(claim)) std::terminate();
            return ReadResult::Finished;
        }
        // Cancel directly without self-notification so WaitSignal does not become a busy retry loop.
        if (!queue_.cancel_claim(claim)) std::terminate();
        return ReadResult::Cancelled;
    }
    void notify_worker() noexcept {
        if (options_.reader_mode != ReaderMode::WaitSignal) return;
        // Publish every enqueue notification even when no waiter exists. A Reader registering afterward observes it
        // through acquire RMW on the same atomic; the wait protocol does not depend on a particular queue's internal lock.
        auto mask = sleepers_.fetch_or(0, std::memory_order_acq_rel);
        while (mask) {
            const auto index = std::countr_zero(mask);
            if (sleepers_.compare_exchange_weak(mask, mask & (mask - 1), std::memory_order_acq_rel)) {
                workers_[index].event.set();
                return;
            }
        }
    }
    static void pause_cpu() noexcept {
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
        _mm_pause();
#elif defined(__i386__) || defined(__x86_64__)
        __builtin_ia32_pause();
#else
        std::this_thread::yield();
#endif
    }
    void run(std::size_t index) noexcept {
        const auto bit = std::uint64_t{1} << index;
        auto& event = workers_[index].event;
        while (!stopping_.load(std::memory_order_acquire)) {
            const auto result = read_once();
            if (result == ReadResult::Finished) continue;
            if (options_.reader_mode == ReaderMode::Polling) {
                pause_cpu();
                continue;
            }
            sleepers_.fetch_or(bit, std::memory_order_acq_rel);
            // Register before checking again; together with notify_worker's RMW, this closes the notification-to-sleep race.
            if (result == ReadResult::Empty && queue_.has_ready()) {
                sleepers_.fetch_and(~bit, std::memory_order_acq_rel);
                continue;
            }
            if (result == ReadResult::Cancelled) (void)event.wait_for(options_.retry_wait, stopping_);
            else (void)event.wait_for(std::chrono::hours(24), stopping_);
            sleepers_.fetch_and(~bit, std::memory_order_acq_rel);
        }
    }
    TQueue& queue_;
    const HandoffOptions options_;
    TCallback callback_;
    Worker* workers_ = nullptr;
    std::atomic<std::uint64_t> sleepers_{0};
    std::atomic<bool> stopping_{false}, running_{false};
    std::atomic<std::uint64_t> requests_{0};
    std::atomic<bool> processing_{false};
};

template<class TQueue, class TCallback>
HandoffReader(TQueue&, HandoffOptions, TCallback) -> HandoffReader<TQueue, TCallback>;

} // namespace TwoBufQueue
