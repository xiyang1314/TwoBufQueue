#pragma once

#include "TwoBufQueue/src/read/read_layer1/read_item.hpp"
#include "TwoBufQueue/src/write/make_queue.hpp"
#include "TwoBufQueue/src/utils/auto_reset_event.hpp"
#include "TwoBufQueue/src/utils/fast_time.hpp"
#include <concepts>

namespace TwoBufQueue {

// 独立的单来源读取服务；构造时绑定，运行期间不注册或切换 Writer。
// Callback / queue / event 的寿命须覆盖本对象；只有一个线程调用 read_once。
template<class TCallback>
class SingleReadService {
public:
    using Queue = WriterQueue;
    using Block = SpscWriteBlock;
    using SteadyClock = ReadItem::SteadyClock;
    using Deadline = AutoResetEvent::Clock::time_point;
    static_assert(requires(TCallback& cb, Block* block, const std::byte* ptr,
                           std::size_t size, std::int64_t time) {
        { cb.begin_read_complete(time) } noexcept -> std::same_as<void>;
        { cb.read_complete(block) } noexcept -> std::same_as<bool>;
        { cb.end_read_complete() } noexcept -> std::same_as<void>;
        { cb.begin_read_current(time) } noexcept -> std::same_as<void>;
        { cb.read_current(block, ptr, size) } noexcept -> std::same_as<bool>;
        { cb.end_read() } noexcept -> std::same_as<void>;
    }, "Callback must implement the noexcept methods in ReadWriterDataCallback");

    SingleReadService(ReadConfig config, TCallback& callback, AutoResetEvent& event,
                      Queue& queue, std::int32_t writer_id)
        : config_(config), callback_(callback), event_(event), queue_(queue),
          writer_id_(writer_id), item_(config) {
        if (writer_id < 0) record_error(ReadError::invalid_registration);
    }
    template<class TWriter>
    SingleReadService(ReadConfig config, TCallback& callback, AutoResetEvent& event, TWriter& writer)
        : SingleReadService(config, callback, event, writer.handoff_queue(), writer.thread_id()) {}
    ~SingleReadService() {
        detach_completed_callbacks();
        if (pending_) {
            std::fputs("destroying SingleReadService with a pending popped record; drain or take_pending first\n", stderr);
            std::terminate();
        }
    }
    SingleReadService(const SingleReadService&) = delete;
    SingleReadService& operator=(const SingleReadService&) = delete;

    // 固定交接前缀，先处理完成块，再按 is_scan_current 选择是否读取 current。
    // force_current 只绕过时间间隔，不绕过 is_scan_current；新通知保留在 event 中。
    [[nodiscard]] ReadResult read_once(bool force_current, const std::atomic<bool>& cancel,
                                       Deadline deadline = Deadline::max()) noexcept {
        ReadResult result;
        if (interrupted(cancel, deadline)) {
            result.pending = true;
        } else {
            std::size_t count = 0;
            auto* current = queue_.snapshot_for_read(count).get();
            count += static_cast<std::size_t>(bool(pending_));
            if (read_completed(count, cancel, deadline, result) && config_.is_scan_current)
                (void)read_current(current, force_current, cancel, deadline, result);
        }
        callback_.end_read();
        result.pending |= bool(pending_) || queue_.pending_count() != 0;
        return result;
    }
    [[nodiscard]] const ReadConfig& config() const noexcept { return config_; }
    [[nodiscard]] bool valid_start_state() const noexcept {
        const auto valid_wait = [](std::chrono::milliseconds value) {
            return value > std::chrono::milliseconds::zero() &&
                value <= std::chrono::duration_cast<std::chrono::milliseconds>(AutoResetEvent::Clock::duration::max());
        };
        return writer_id_ >= 0 && config_.idle_rescan_interval.count() > 0 &&
            config_.idle_rescan_max_interval >= config_.idle_rescan_interval &&
            valid_wait(config_.finish_timeout) && valid_wait(config_.work_wait_min_interval) &&
            valid_wait(config_.finish_retry_interval);
    }
    // 安装/解除回调时生产者须静止，与多 Writer 的生命周期约束相同。
    void prepare_start() noexcept {
        queue_.set_read_available_callback(&read_available, &event_);
        attached_ = true;
    }
    void detach_completed_callbacks() noexcept {
        if (!attached_) return;
        queue_.set_read_available_callback(nullptr, nullptr);
        attached_ = false;
    }
    [[nodiscard]] Queue::Record take_pending() noexcept {
        if (attached_) return {};
        auto record = pending_;
        pending_ = {};
        popped_pending_count_.store(0, std::memory_order_relaxed);
        return record;
    }
    [[nodiscard]] bool adopt_pending(Queue::Record& record) noexcept {
        if (attached_ || pending_ || !queue_.is_pending(record)) return false;
        pending_ = record;
        record = {};
        popped_pending_count_.store(1, std::memory_order_relaxed);
        return true;
    }
    [[nodiscard]] bool has_pending_finished() const noexcept { return pending_finished_count() != 0; }
    [[nodiscard]] std::size_t pending_finished_count() const noexcept {
        return popped_pending_count_.load(std::memory_order_relaxed) + queue_.pending_count();
    }
    [[nodiscard]] SteadyClock::duration next_scan_deadline() const noexcept {
        if (!config_.is_scan_current) return config_.idle_rescan_interval;
        const auto now = SteadyClock::now();
        return item_.next_read_time() <= now ? SteadyClock::duration::zero() : item_.next_read_time() - now;
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
    bool read_completed(std::size_t count, const std::atomic<bool>& cancel, Deadline deadline,
                        ReadResult& result) noexcept {
        bool began = false;
        const auto end_phase = [&](bool ok) {
            if (began) callback_.end_read_complete();
            return ok;
        };
        for (std::size_t i = 0; i < count; ++i) {
            if (interrupted(cancel, deadline)) { result.pending = true; return end_phase(false); }
            if (!pending_) {
                pending_ = queue_.try_pop();
                if (!pending_) {
                    result.error = ReadError::foreign_queued_range;
                    result.pending = true;
                    record_error(result.error);
                    return end_phase(false);
                }
                popped_pending_count_.store(1, std::memory_order_relaxed);
            }
            auto* block = pending_.get();
            if (!block) { finish_pending(); result.progressed = true; continue; }
            const auto bytes = block->readable();
            if (!bytes.empty()) {
                block->source_id_ = writer_id_;
                if (!began) {
                    callback_.begin_read_complete(fast_epoch_time_ns());
                    began = true;
                    if (interrupted(cancel, deadline)) { result.pending = true; return end_phase(false); }
                }
                if (!callback_.read_complete(block)) {
                    record_rejected(*block, bytes.data());
                    result.pending = true;
                    return end_phase(false);
                }
                block->read_pos_ = static_cast<std::size_t>(bytes.data() + bytes.size() - block->buf_);
                record_saved(bytes.size(), false);
            }
            finish_pending(); // block 可立即复用，之后不再访问它。
            increment(saved_finished_ranges_);
            result.progressed = true;
        }
        return end_phase(true);
    }
    void finish_pending() noexcept {
        queue_.finish(pending_);
        popped_pending_count_.store(0, std::memory_order_relaxed);
    }
    bool read_current(Block* block, bool force, const std::atomic<bool>& cancel,
                      Deadline deadline, ReadResult& result) noexcept {
        if (interrupted(cancel, deadline)) { result.pending = true; return false; }
        if (!block) {
            (void)item_.should_read_current(SteadyClock::now(), force, 0);
            return true;
        }
        const auto* read_ptr = block->buf_ + block->read_pos_;
        const auto* write_ptr = block->write_ptr_.load(std::memory_order_acquire);
        const auto size = static_cast<std::size_t>(write_ptr - read_ptr);
        if (!item_.should_read_current(SteadyClock::now(), force, size)) {
            result.pending |= size != 0;
            return true;
        }
        block->source_id_ = writer_id_;
        callback_.begin_read_current(fast_epoch_time_ns());
        if (interrupted(cancel, deadline)) { result.pending = true; return false; }
        if (!callback_.read_current(block, read_ptr, size)) {
            record_rejected(*block, read_ptr);
            result.pending = true;
            return false;
        }
        block->read_pos_ = static_cast<std::size_t>(write_ptr - block->buf_);
        record_saved(size, true);
        result.progressed = true;
        return true;
    }
    // 这些计数只有读线程写；原子 load/store 允许控制线程查询，避免不必要的 RMW。
    static void increment(std::atomic<std::uint64_t>& counter, std::uint64_t amount = 1) noexcept {
        counter.store(counter.load(std::memory_order_relaxed) + amount, std::memory_order_relaxed);
    }
    void record_saved(std::size_t bytes, bool current) noexcept {
        rejected_block_ = 0;
        increment(saved_frames_);
        increment(saved_data_bytes_, bytes);
        if (current) increment(saved_partial_frames_);
    }
    void record_rejected(const Block& block, const std::byte* ptr) noexcept {
        increment(memory_full_events_);
        const auto id = reinterpret_cast<std::uintptr_t>(&block);
        const auto offset = static_cast<std::size_t>(ptr - block.buf_);
        if (rejected_block_ == id && rejected_sequence_ == block.sequence_ && rejected_offset_ == offset) return;
        rejected_block_ = id;
        rejected_sequence_ = block.sequence_;
        rejected_offset_ = offset;
        increment(memory_backpressured_frames_);
    }
    static bool interrupted(const std::atomic<bool>& cancel, Deadline deadline) noexcept {
        return cancel.load(std::memory_order_acquire) ||
            (deadline != Deadline::max() && AutoResetEvent::Clock::now() >= deadline);
    }
    static void read_available(void* context) noexcept {
        static_cast<AutoResetEvent*>(context)->set();
    }

    const ReadConfig config_;
    TCallback& callback_;
    AutoResetEvent& event_;
    Queue& queue_;
    const std::int32_t writer_id_;
    ReadItem item_;
    Queue::Record pending_;
    bool attached_ = false;
    std::uintptr_t rejected_block_ = 0;
    std::uint64_t rejected_sequence_ = 0;
    std::size_t rejected_offset_ = 0;
    std::atomic<std::size_t> popped_pending_count_{0};
    std::atomic<std::uint64_t> saved_frames_{0}, saved_data_bytes_{0}, saved_finished_ranges_{0}, saved_partial_frames_{0};
    std::atomic<std::uint64_t> memory_full_events_{0}, memory_backpressured_frames_{0}, error_count_{0};
    std::atomic<ReadError> error_code_{ReadError::none};
};
} // namespace TwoBufQueue
