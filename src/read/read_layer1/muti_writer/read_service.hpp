#pragma once
#include "TwoBufQueue/src/read/read_layer1/read_item.hpp"
#include "TwoBufQueue/src/write/make_queue.hpp"
#include "TwoBufQueue/src/utils/auto_reset_event.hpp"
#include "TwoBufQueue/src/utils/fast_time.hpp"
#include <algorithm>
#include <bit>
#include <concepts>
#include <memory>
#include <vector>

namespace TwoBufQueue {
// Callback is the user's concrete type, without inheritance; one reader invokes every callback synchronously.
template<class Callback>
class ReadService {
public:
    using Queue = WriterQueue;
    using Block = SpscWriteBlock;
    using SteadyClock = ReadItem::SteadyClock;
    using Deadline = AutoResetEvent::Clock::time_point;
    static_assert(requires(Callback& cb, Block* block, const std::byte* ptr,
                           std::size_t size, std::int64_t time) {
        { cb.begin_read_complete(time) } noexcept -> std::same_as<void>;
        { cb.read_complete(block) } noexcept -> std::same_as<bool>;
        { cb.end_read_complete() } noexcept -> std::same_as<void>;
        { cb.begin_read_current(time) } noexcept -> std::same_as<void>;
        { cb.read_current(block, ptr, size) } noexcept -> std::same_as<bool>;
        { cb.end_read() } noexcept -> std::same_as<void>;
    }, "Callback must implement the noexcept block-reading methods shown in ReadWriterDataCallback");

    ReadService(ReadConfig config, Callback& callback, AutoResetEvent& work_event)
        : config_(config), callback_(callback), work_event_(work_event) {}
    ~ReadService() {
        detach_completed_callbacks();
        if (popped_pending_count_.load(std::memory_order_relaxed)) {
            std::fputs("destroying ReadService with a pending popped record; drain or take_pending first\n", stderr);
            std::terminate();
        }
    }
    ReadService(const ReadService&) = delete;
    ReadService& operator=(const ReadService&) = delete;

    template<class Writer> [[nodiscard]] bool add_writer(Writer* writer, bool running = false) noexcept {
        return add_queue(writer ? &writer->handoff_queue() : nullptr,
                         writer ? writer->thread_id() : -1, running);
    }
    [[nodiscard]] bool add_writer(std::nullptr_t, bool = false) noexcept {
        record_error(ReadError::null_dependency); return false;
    }
    [[nodiscard]] bool add_queue(Queue* queue, std::int32_t writer_id, bool running = false) noexcept {
        if (!queue) { record_error(ReadError::null_dependency); return false; }
        if (running || attached_ || writer_id < 0) {
            record_error(ReadError::invalid_registration); return false;
        }
        for (const auto& entry : queues_) {
            if (entry.queue == queue || entry.writer_id == writer_id) {
                record_error(ReadError::duplicate_writer); return false;
            }
        }
        try {
            selected_.reserve(queues_.size() + 1);
            if (notifications_.size() < (queues_.size() + 64) / 64)
                notifications_.push_back(std::make_unique<NotificationWord>());
            queues_.emplace_back(queue, writer_id, config_,
                                notifications_[queues_.size() / 64].get(), queues_.size() % 64, &work_event_);
            next_full_scan_time_ = {};
            return true;
        }
        catch (...) { record_error(ReadError::registration_allocation_failed); return false; }
    }

    // Process pending plus the prefix fixed by prepare_read; retain popped records if a completed-block callback fails.
    // is_scan_current = false processes completed blocks only; force_current does not bypass this setting.
    [[nodiscard]] ReadResult read_once(bool force_current, const std::atomic<bool>& cancel,
                                       Deadline deadline = Deadline::max()) noexcept {
        selected_.clear();
        for (std::size_t i = 0; i < queues_.size(); ++i) {
            selected_.push_back(i);
            queues_[i].notified = false;
        }
        // Consume old notifications before fixing snapshots; notifications arriving during reads belong to the next round.
        for (std::size_t word = 0; word < notifications_.size(); ++word) {
            auto bits = notifications_[word]->bits.exchange(0, std::memory_order_acq_rel);
            while (bits) {
                queues_[word * 64 + std::countr_zero(bits)].notified = true;
                bits &= bits - 1;
            }
        }
        auto result = read_selected(force_current, cancel, deadline);
        next_full_scan_time_ = SteadyClock::time_point::max();
        for (const auto& entry : queues_) {
            if (config_.is_scan_current)
                next_full_scan_time_ = std::min(next_full_scan_time_, entry.item.next_read_time());
            if (entry.notified) mark_notified(entry.notification);
        }
        return result;
    }

    // An event means notifications exist; the bitmap retains every source. Coalesce repeated source notifications without limiting Writer count.
    [[nodiscard]] ReadResult read_notified(const std::atomic<bool>& cancel,
                                           Deadline deadline = Deadline::max()) noexcept {
        selected_.clear();
        for (std::size_t word = 0; word < notifications_.size(); ++word) {
            auto bits = notifications_[word]->bits.exchange(0, std::memory_order_acq_rel);
            while (bits) {
                selected_.push_back(word * 64 + std::countr_zero(bits));
                queues_[selected_.back()].notified = true;
                bits &= bits - 1;
            }
        }
        auto result = read_selected(true, cancel, deadline);
        for (auto index : selected_) {
            auto& entry = queues_[index];
            // A targeted read must not postpone other sources' deadlines; one conservative early full scan is acceptable.
            if (config_.is_scan_current)
                next_full_scan_time_ = std::min(next_full_scan_time_, entry.item.next_read_time());
            // Retain sources after backpressure/abort, without self-waking into a no-progress busy loop.
            if (entry.notified || entry.pending || entry.queue->pending_count())
                mark_notified(entry.notification);
        }
        return result;
    }

    [[nodiscard]] const ReadConfig& config() const noexcept { return config_; }
    [[nodiscard]] std::size_t writer_count() const noexcept { return queues_.size(); }
    [[nodiscard]] bool valid_start_state() const noexcept {
        const auto valid_wait = [](std::chrono::milliseconds value) {
            return value > std::chrono::milliseconds::zero() &&
                value <= std::chrono::duration_cast<std::chrono::milliseconds>(AutoResetEvent::Clock::duration::max());
        };
        return !queues_.empty() && config_.idle_rescan_interval.count() > 0 &&
            config_.idle_rescan_max_interval >= config_.idle_rescan_interval &&
            valid_wait(config_.finish_timeout) && valid_wait(config_.work_wait_min_interval) &&
            valid_wait(config_.finish_retry_interval);
    }
    void prepare_start() noexcept {
        for (auto& entry : queues_)
            entry.queue->set_read_available_callback(&read_available, &entry.notification);
        attached_ = true;
    }
    void detach_completed_callbacks() noexcept {
        if (!attached_) return;
        for (auto& entry : queues_) entry.queue->set_read_available_callback(nullptr, nullptr);
        attached_ = false;
    }
    // Call only after reads stop; transfer popped records without returning resources or enqueueing again.
    [[nodiscard]] Queue::Record take_pending(Queue* queue) noexcept {
        if (attached_) return {};
        for (auto& entry : queues_) if (entry.queue == queue && entry.pending) {
            auto record = entry.pending;
            entry.pending = {};
            popped_pending_count_.fetch_sub(1, std::memory_order_relaxed);
            return record;
        }
        return {};
    }
    // Register the same queue on the new reader before adopting old pending; success clears the caller's ticket.
    [[nodiscard]] bool adopt_pending(Queue* queue, Queue::Record& record) noexcept {
        if (attached_ || !queue || !queue->is_pending(record)) return false;
        for (auto& entry : queues_) if (entry.queue == queue && !entry.pending) {
            entry.pending = record;
            record = {};
            popped_pending_count_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }
    [[nodiscard]] bool has_pending_finished() const noexcept {
        if (popped_pending_count_.load(std::memory_order_relaxed)) return true;
        for (const auto& entry : queues_) if (entry.queue->pending_count()) return true;
        return false;
    }
    [[nodiscard]] std::size_t pending_finished_count() const noexcept {
        std::size_t count = popped_pending_count_.load(std::memory_order_relaxed);
        for (const auto& entry : queues_) count += entry.queue->pending_count();
        return count; // Includes queued begin records and pending records already popped but not completed.
    }
    [[nodiscard]] SteadyClock::duration next_scan_deadline() const noexcept {
        if (!config_.is_scan_current) return config_.idle_rescan_interval;
        const auto now = SteadyClock::now();
        return next_full_scan_time_ == SteadyClock::time_point::max() || next_full_scan_time_ <= now
            ? SteadyClock::duration::zero() : next_full_scan_time_ - now;
    }
    [[nodiscard]] ReadStats stats() const noexcept {
        ReadStats result;
        result.saved_frames = saved_frames_.load(std::memory_order_relaxed);
        result.saved_data_bytes = saved_data_bytes_.load(std::memory_order_relaxed);
        result.saved_finished_ranges = saved_finished_ranges_.load(std::memory_order_relaxed);
        result.saved_partial_frames = saved_partial_frames_.load(std::memory_order_relaxed);
        result.memory_full_events = memory_full_events_.load(std::memory_order_relaxed);
        result.memory_backpressured_frames = memory_backpressured_frames_.load(std::memory_order_relaxed);
        result.error_count = error_count_.load(std::memory_order_relaxed);
        return result;
    }
    [[nodiscard]] ReadError error_code() const noexcept { return error_code_.load(std::memory_order_relaxed); }
    void record_error(ReadError error) noexcept {
        if (error == ReadError::none) return;
        error_code_.store(error, std::memory_order_relaxed);
        error_count_.fetch_add(1, std::memory_order_relaxed);
    }

private:
    struct alignas(64) NotificationWord { std::atomic<std::uint64_t> bits{0}; };
    // Callback context becomes read-only after registration and is separate from consumption cursors; shared updates use separate notification words.
    struct alignas(64) NotificationContext {
        NotificationWord* word;
        std::uint64_t bit;
        AutoResetEvent* event;
    };
    struct QueueEntry {
        QueueEntry(Queue* source, std::int32_t id, const ReadConfig& config,
                   NotificationWord* word, std::size_t bit, AutoResetEvent* event)
            : queue(source), writer_id(id), item(config), notification{word, std::uint64_t{1} << bit, event} {}
        Queue* queue;
        std::int32_t writer_id;
        ReadItem item;
        NotificationContext notification;
        bool notified = false; // Reader only: when a full scan observes a notification, read that source's current immediately too.
        Queue::Record pending; // Already dequeued; this reader retains it on failure/stop and processes it first next time.
        std::size_t finish_count = 0; // Fixed handoff-record count for this round, including begin records.
        Block* current = nullptr; // next at the end of the queue prefix; an empty snapshot uses queue.current().
        // Service statistics deduplication key; does not select reads or prioritize retries.
        std::uintptr_t rejected_block = 0;
        std::uint64_t rejected_sequence = 0;
        std::size_t rejected_offset = 0;
    };

    [[nodiscard]] ReadResult read_selected(bool force_current, const std::atomic<bool>& cancel,
                                           Deadline deadline) noexcept {
        ReadResult result;
        if (interrupted(cancel, deadline)) {
            result.pending = true;
        } else {
            prepare_read();
            if (read_completed(cancel, deadline, result)) {
                if (config_.is_scan_current) {
                    (void)read_current(force_current, cancel, deadline, result);
                } else {
                    // Completed records and flush notification have been handled; retain the source for retry on failure/abort.
                    for (auto index : selected_) queues_[index].notified = false;
                }
            }
        }
        callback_.end_read();
        // Newly enqueued records belong to the next round; after production stops this result can determine drain completion.
        for (auto index : selected_) {
            const auto& entry = queues_[index];
            result.pending |= bool(entry.pending) || entry.queue->pending_count() != 0;
        }
        return result;
    }
    // Fix snapshots before reading any selected source; later enqueues belong to the next round.
    void prepare_read() noexcept {
        for (auto index : selected_) {
            auto& entry = queues_[index];
            // With completed records, use this round's final next; otherwise queue.current(). Never access Writer.
            entry.current = entry.queue->snapshot_for_read(entry.finish_count).get();
            entry.finish_count += static_cast<std::size_t>(bool(entry.pending));
        }
    }

    bool read_completed(const std::atomic<bool>& cancel, Deadline deadline, ReadResult& result) noexcept {
        bool began = false;
        const auto end_phase = [&](bool ok) {
            if (began) callback_.end_read_complete();
            return ok;
        };
        for (auto index : selected_) {
            if (!read_completed_source(queues_[index], cancel, deadline, result, began))
                return end_phase(false);
        }
        return end_phase(true);
    }

    bool read_completed_source(QueueEntry& entry, const std::atomic<bool>& cancel, Deadline deadline,
                               ReadResult& result, bool& began) noexcept {
        for (std::size_t i = 0; i < entry.finish_count; ++i) {
            if (interrupted(cancel, deadline)) { result.pending = true; return false; }
            // For each source, handle its retained pending before popping this round's fixed prefix.
            if (!entry.pending) {
                entry.pending = entry.queue->try_pop();
                if (!entry.pending) return fail(result, ReadError::foreign_queued_range);
                popped_pending_count_.fetch_add(1, std::memory_order_relaxed);
            }
            auto* block = entry.pending.get();
            if (!block) {
                finish_pending(entry);
                result.progressed = true;
                continue; // {empty, next} advances the queue only; neither begin a completed-block callback nor count a completed block.
            }
            const auto bytes = block->readable(); // A completed block's write position is already fixed.
            if (!bytes.empty()) {
                // Source identifier supplied at queue registration, not an operating-system thread id.
                // The callback receives only Block; block->source_id() identifies its Writer.
                // Only the reader writes this metadata; Writer does not read it and the pool resets it only during reuse.
                block->source_id_ = entry.writer_id;
                if (!began) {
                    callback_.begin_read_complete(fast_epoch_time_ns()); began = true;
                    // begin is user code and may take time or request stop; recheck once after invoking it.
                    if (interrupted(cancel, deadline)) {
                        result.pending = true; return false;
                    }
                }

                if (!callback_.read_complete(block)) {
                    record_rejected(entry, *block, bytes.data());
                    result.pending = true;
                    return false;
                }
                block->read_pos_ = static_cast<std::size_t>(bytes.data() + bytes.size() - block->buf_);
                record_saved(entry, bytes.size(), false);
            }
            finish_pending(entry);
            // The block may already have been reused; update only local state/statistics afterward.
            saved_finished_ranges_.fetch_add(1, std::memory_order_relaxed);
            result.progressed = true;
        }
        return true;
    }

    void finish_pending(QueueEntry& entry) noexcept {
        entry.queue->finish(entry.pending);
        popped_pending_count_.fetch_sub(1, std::memory_order_relaxed);
    }

    bool read_current(bool force, const std::atomic<bool>& cancel, Deadline deadline,
                      ReadResult& result) noexcept {
        bool began = false;
        for (auto index : selected_)
            if (!read_current_source(queues_[index], force || queues_[index].notified,
                                     cancel, deadline, result, began)) return false;
        return true;
    }

    bool read_current_source(QueueEntry& entry, bool force, const std::atomic<bool>& cancel,
                             Deadline deadline, ReadResult& result, bool& began) noexcept {
        if (interrupted(cancel, deadline)) { result.pending = true; return false; }
        auto* block = entry.current;
        if (!block) {
            // No current also counts as an empty scan, preventing this source from polling forever at the minimum interval.
            (void)entry.item.should_read_current(SteadyClock::now(), force, 0);
            entry.notified = false;
            return true;
        }
        // Read the consumption cursor and shared publication cursor once each; callbacks and acknowledgement share this snapshot.
        const auto* read_ptr = block->buf_ + block->read_pos_;
        const auto* write_ptr = block->write_ptr_.load(std::memory_order_acquire);
        const auto read_size = static_cast<std::size_t>(write_ptr - read_ptr);
        if (!entry.item.should_read_current(SteadyClock::now(), force, read_size)) {
            result.pending |= read_size != 0;
            entry.notified = false;
            return true;
        }
        block->source_id_ = entry.writer_id;
        if (!began) {
            callback_.begin_read_current(fast_epoch_time_ns()); began = true;
            if (interrupted(cancel, deadline)) { result.pending = true; return false; }
        }
        if (!callback_.read_current(block, read_ptr, read_size)) {
            record_rejected(entry, *block, read_ptr);
            result.pending = true; return false;
        }
        // Do not call consume() to reread write_ptr; acknowledge only the exact range passed to business code.
        block->read_pos_ = static_cast<std::size_t>(write_ptr - block->buf_);
        record_saved(entry, read_size, true);
        entry.notified = false;
        result.progressed = true;
        return true;
    }

    void record_saved(QueueEntry& entry, std::size_t bytes, bool current) noexcept {
        entry.rejected_block = 0;
        saved_frames_.fetch_add(1, std::memory_order_relaxed);
        saved_data_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        if (current) saved_partial_frames_.fetch_add(1, std::memory_order_relaxed);
    }
    void record_rejected(QueueEntry& entry, const Block& block, const std::byte* read_ptr) noexcept {
        memory_full_events_.fetch_add(1, std::memory_order_relaxed);
        const auto id = reinterpret_cast<std::uintptr_t>(&block);
        const auto offset = static_cast<std::size_t>(read_ptr - block.buf_);
        if (entry.rejected_block == id && entry.rejected_sequence == block.sequence_ &&
            entry.rejected_offset == offset) return;
        entry.rejected_block = id;
        entry.rejected_sequence = block.sequence_;
        entry.rejected_offset = offset;
        memory_backpressured_frames_.fetch_add(1, std::memory_order_relaxed);
    }
    bool fail(ReadResult& result, ReadError error) noexcept {
        result.error = error; result.pending = true; record_error(error); return false;
    }
    static bool interrupted(const std::atomic<bool>& cancel, Deadline deadline) noexcept {
        return cancel.load(std::memory_order_acquire) ||
            (deadline != Deadline::max() && AutoResetEvent::Clock::now() >= deadline);
    }
    static void mark_notified(const NotificationContext& notification) noexcept {
        notification.word->bits.fetch_or(notification.bit, std::memory_order_release);
    }
    static void read_available(void* context) noexcept {
        const auto& notification = *static_cast<NotificationContext*>(context);
        mark_notified(notification);
        // Set the event even if the bit was set: it may be retained for backpressure retry and must not suppress a new production notification.
        notification.event->set();
    }

    const ReadConfig config_;
    Callback& callback_;
    AutoResetEvent& work_event_;
    std::vector<QueueEntry> queues_;
    std::vector<std::unique_ptr<NotificationWord>> notifications_;
    std::vector<std::size_t> selected_; // Reserve at registration; no allocations in the read path.
    SteadyClock::time_point next_full_scan_time_{};
    bool attached_ = false;
    std::atomic<std::size_t> popped_pending_count_{0};
    std::atomic<std::uint64_t> saved_frames_{0}, saved_data_bytes_{0}, saved_finished_ranges_{0}, saved_partial_frames_{0};
    std::atomic<std::uint64_t> memory_full_events_{0}, memory_backpressured_frames_{0}, error_count_{0};
    std::atomic<ReadError> error_code_{ReadError::none};
};
} // namespace TwoBufQueue
