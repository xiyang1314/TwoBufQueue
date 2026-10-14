#pragma once

#include "TwoBufQueue/src/read/read_layer1/types.hpp"
#include "TwoBufQueue/src/utils/auto_reset_event.hpp"
#include "TwoBufQueue/src/utils/brqHelp.hpp"
#include "TwoBufQueue/src/read/read_layer1/muti_writer/read_service.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <mutex>
#include <string>
#include <thread>

namespace TwoBufQueue {

// Manages threads, wakeups and stop only; ReadService<Callback> implements the read flow.
template<class Callback>
class ReadWorker {
public:
    // config supplies every scan/wait/drain duration; callers may override all defaults.
    // Store configuration without starting a thread; register every writer before start.
    // The caller owns callback and must keep it alive beyond Worker; no interface inheritance required.
    explicit ReadWorker(ReadConfig config, Callback& callback);
    // Stop and join the internal thread; after fast stop/timeout, restart to drain or transfer pending with take_pending before destruction.
    ~ReadWorker() noexcept;

    // Do not copy the reader thread or its bound writer state.
    ReadWorker(const ReadWorker&) = delete;
    // Disable copy assignment so multiple workers cannot take ownership of the same completion callback.
    ReadWorker& operator=(const ReadWorker&) = delete;

    // Register a Writer with any publication count; call only before start.
    // WriterT accepts Writer or a WriterBase derivative and uses public read-only interfaces only.
    template<class WriterT>
    [[nodiscard]] bool add_writer(WriterT* writer) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return service_.add_writer(writer, running_.load(std::memory_order_acquire));
    }
    [[nodiscard]] bool add_queue(WriterQueue* queue, std::int32_t thread_id) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return service_.add_queue(queue, thread_id, running_.load(std::memory_order_acquire));
    }
    // Explicitly reject a null-writer overload to simplify uniform caller error paths.
    [[nodiscard]] bool add_writer(std::nullptr_t) noexcept;
    // Validate state, install completion callbacks and start the background reader; repeated start succeeds.
    [[nodiscard]] bool start() noexcept;

    // Before stop, stop producer writes, finish flushing and wait for existing completed-block callbacks to return.
    // With is_scan_current = false, finish the Writer to move its incomplete tail into the completed queue.
    // slow (default): collect remaining source data and pass it to the callback; wait for downstream for at most finish_timeout.
    // On timeout record finish_timeout and exit; stop no longer guarantees collection of all source data.
    // fast: stop collecting after the running synchronous callback returns; do not wait for downstream capacity.
    // Both modes join the reader thread and detach writer callbacks.
    // External storage must then finish itself and expose its own drain state for downstream resource release.
    // A control thread serializes calls; never run multiple stop calls concurrently.
    void stop(ReadStopMode mode = ReadStopMode::slow) noexcept;

    // Nonblocking stop request: set the condition and wake ordinary/drain waits; neither join nor detach callbacks.
    // During another control thread's stop(slow), request_stop(fast) may interrupt draining.
    // A later slow request cannot downgrade fast stop; one control thread must still call stop to join/clean up.
    // Must not overlap start/destruction; stop writes and flush before requesting slow stop as well.
    void request_stop(ReadStopMode mode = ReadStopMode::slow) noexcept;

    // Notify the reader after downstream releases a batch so it retries backpressured SpscWriteBlocks.
    void notify_memory_available() noexcept;

    // Whether the background reader started successfully and has not fully stopped.
    [[nodiscard]] bool running() const noexcept;
    // Number of writers registered before start.
    [[nodiscard]] std::size_t writer_count() const noexcept;
    // Pending record count: queued records including begins, plus pending retained by Service.
    [[nodiscard]] std::size_t pending_finished_count() const noexcept;

    // Transfer pending after stop/join, or adopt another reader's pending before start.
    [[nodiscard]] WriterQueue::Record take_pending(WriterQueue* queue) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.load(std::memory_order_acquire)) return {};
        return service_.take_pending(queue);
    }
    [[nodiscard]] bool adopt_pending(WriterQueue* queue, WriterQueue::Record& record) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.load(std::memory_order_acquire)) return false;
        return service_.adopt_pending(queue, record);
    }
    // Aggregate read, cursor-acknowledgement and backpressure statistics; external storage provides its own statistics.
    [[nodiscard]] ReadStats stats() const noexcept;
    // Atomically query the most recent recorded error; initially none. Successful calls do not clear historical errors.
    [[nodiscard]] ReadError error_code() const noexcept;
    // Compatibility text interface: convert the enum when queried, without storing an error string.
    [[nodiscard]] std::string last_error() const;

private:
    using SteadyClock = FastSteadyClock;
    void run() noexcept;
    void finish_remaining_work() noexcept;
    [[nodiscard]] bool spin_for_completed_work() noexcept;
    [[nodiscard]] bool wait_for_work() noexcept;
    void notify_worker() noexcept;

    // Serialize registration, start and stop; each event manages its own wait synchronization.
    mutable std::mutex mutex_;
    // Construct the event first and destroy it last so Service's event reference stays valid throughout its lifetime.
    AutoResetEvent work_event_;
    ReadService<Callback> service_;
    std::thread worker_thread_;
    // Worker owns thread execution, notifications and stop requests; Service's Items own per-writer state.
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> memory_retry_requested_{false};
    // Set by stop/request_stop(fast), cleared only by start; fast stop interrupts draining between Item operations.
    std::atomic<bool> fast_stop_requested_{false};
};

template<class Callback>
ReadWorker<Callback>::ReadWorker(ReadConfig config, Callback& callback)
    : service_(config, callback, work_event_) {}

template<class Callback>
ReadWorker<Callback>::~ReadWorker() noexcept { stop(); }

template<class Callback>
bool ReadWorker<Callback>::add_writer(std::nullptr_t writer) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return service_.add_writer(writer, running_.load(std::memory_order_acquire));
}

// Install every block-switch callback and start the reader; roll back every installation if thread creation fails.
template<class Callback>
bool ReadWorker<Callback>::start() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_.load(std::memory_order_acquire)) return true;
    if (!service_.valid_start_state()) {
        service_.record_error(ReadError::invalid_start_state);
        return false;
    }
    initialize_fast_epoch_clock();
    stop_requested_.store(false, std::memory_order_relaxed);
    fast_stop_requested_.store(false, std::memory_order_relaxed);
    memory_retry_requested_.store(false, std::memory_order_relaxed);
    work_event_.reset();
    service_.prepare_start();
    running_.store(true, std::memory_order_release);
    try {
        worker_thread_ = std::thread(&ReadWorker<Callback>::run, this);
    } catch (...) {
        running_.store(false, std::memory_order_release);
        service_.detach_completed_callbacks();
        service_.record_error(ReadError::thread_creation_failed);
        return false;
    }
    return true;
}

// Follow the requested slow/fast stop mode, join the thread and detach writer callbacks.
template<class Callback>
void ReadWorker<Callback>::stop(ReadStopMode mode) noexcept {
    if (!running_.load(std::memory_order_acquire)) return;
    request_stop(mode);
    if (worker_thread_.joinable()) worker_thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    service_.detach_completed_callbacks();
    running_.store(false, std::memory_order_release);
}

// Publish the stop condition before setting the event: sleeping threads wake and not-yet-waiting threads retain the signal.
template<class Callback>
void ReadWorker<Callback>::request_stop(ReadStopMode mode) noexcept {
    if (!running_.load(std::memory_order_acquire)) return;
    if (mode == ReadStopMode::fast)
        fast_stop_requested_.store(true, std::memory_order_release);
    stop_requested_.store(true, std::memory_order_release);
    notify_worker();
}

// Set the event when downstream frees space; retain the signal even before the reader waits.
template<class Callback>
void ReadWorker<Callback>::notify_memory_available() noexcept {
    memory_retry_requested_.store(true, std::memory_order_release);
    notify_worker();
}

// acquire-load the worker-running flag as an instantaneous control-plane observation.
template<class Callback>
bool ReadWorker<Callback>::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

template<class Callback>
std::size_t ReadWorker<Callback>::writer_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return service_.writer_count();
}

template<class Callback>
std::size_t ReadWorker<Callback>::pending_finished_count() const noexcept {
    return service_.pending_finished_count();
}

template<class Callback>
ReadStats ReadWorker<Callback>::stats() const noexcept {
    return service_.stats();
}

template<class Callback>
ReadError ReadWorker<Callback>::error_code() const noexcept {
    return service_.error_code();
}

template<class Callback>
std::string ReadWorker<Callback>::last_error() const {
    return error_message(error_code());
}

// Main loop: save completed blocks -> scan active blocks -> bounded spin -> wait for event/timeout.
template<class Callback>
void ReadWorker<Callback>::run() noexcept {
    // Attempt CPU binding.
    if (!pin_current_thread(service_.config().worker_cpu)) {
        service_.record_error(ReadError::cpu_affinity_failed);
    }
    bool scan_all = true;
    for (;!stop_requested_.load(std::memory_order_acquire);) {
        const bool retry_memory = memory_retry_requested_.exchange(false, std::memory_order_acq_rel);
        // Check deadlines even under continuous notifications so unnotified sources are not starved of scans.
        scan_all |= retry_memory || service_.next_scan_deadline() <= SteadyClock::duration::zero();
        const auto result = scan_all ? service_.read_once(retry_memory, stop_requested_)
                                     : service_.read_notified(stop_requested_);
        if (result.error != ReadError::none) return;
        // Continue immediately when a round progresses with work remaining; retry no-progress backpressure on event/timeout.
        if (result.progressed && result.pending) continue;

        // Spin briefly; new work starts the next round.
        if (spin_for_completed_work()) {
            scan_all = false;
            continue;
        }

        // Wait for data/space/control event or timeout; when idle use the nearest scan deadline.
        scan_all = !wait_for_work();
    }

    finish_remaining_work();
}

// Slow stop collects source data under one deadline; fast stop ends collection. External storage publishes output data.
template<class Callback>
void ReadWorker<Callback>::finish_remaining_work() noexcept {
    using Clock = AutoResetEvent::Clock;
    const auto now = Clock::now();
    const auto timeout = service_.config().finish_timeout;
    const auto deadline = now >= Clock::time_point::max() - timeout
        ? Clock::time_point::max() : now + timeout;

    for (;;) {
        // stop_requested_ is already set before draining; draining responds only to new fast-stop requests.
        if (fast_stop_requested_.load(std::memory_order_acquire)) {
            return;
        }
        if (Clock::now() >= deadline) {
            service_.record_error(ReadError::finish_timeout);
            return;
        }

        const auto result = service_.read_once(true, fast_stop_requested_, deadline);
        if (result.error != ReadError::none || fast_stop_requested_.load(std::memory_order_acquire) ||
            !result.pending) return;

        // Retry using constructor configuration; space/stop events wake early without extending the overall deadline.
        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero()) continue;
        const Clock::duration retry_interval = service_.config().finish_retry_interval;
        (void)work_event_.wait_for(std::min(retry_interval, remaining), fast_stop_requested_);
    }
}

// Bounded spin checks stop requests and tries to consume an event; a hit returns to the main loop.
template<class Callback>
bool ReadWorker<Callback>::spin_for_completed_work() noexcept {
    for (std::uint32_t spin = 0; spin < service_.config().idle_spin_count; ++spin) {
        if (stop_requested_.load(std::memory_order_acquire)) return true;
        if (work_event_.try_wait()) return true; // Return when data arrives.
        cpu_relax();
    }
    return false;
}

// Wait for the larger of the configured minimum and remaining scan delay; an event can wake early.
template<class Callback>
bool ReadWorker<Callback>::wait_for_work() noexcept {
    // The event checks the stop condition internally; request_stop sets the condition then the event to wake blocked waits.
    const SteadyClock::duration minimum_wait = service_.config().work_wait_min_interval;
    return work_event_.wait_for(std::max(minimum_wait, service_.next_scan_deadline()),
                               stop_requested_);
}

template<class Callback>
void ReadWorker<Callback>::notify_worker() noexcept {
    work_event_.set();
}


}  // namespace TwoBufQueue
