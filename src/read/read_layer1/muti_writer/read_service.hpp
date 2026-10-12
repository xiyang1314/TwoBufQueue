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
// Callback 是用户自己的类型，不要求继承；所有回调均由唯一读取线程同步调用。
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

    // 一轮读取已有 pending 和 prepare_read 固定的前缀；完成块失败时保留已出队记录。
    // is_scan_current=false 时仅处理完成块；force_current 不绕过此开关。
    [[nodiscard]] ReadResult read_once(bool force_current, const std::atomic<bool>& cancel,
                                       Deadline deadline = Deadline::max()) noexcept {
        selected_.clear();
        for (std::size_t i = 0; i < queues_.size(); ++i) {
            selected_.push_back(i);
            queues_[i].notified = false;
        }
        // 先取走旧通知，再固定快照；读取期间的新通知留给下一轮。
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

    // 事件只表示有通知；位图保留全部来源。同一来源重复通知合并，不限制 Writer 数量。
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
            // 定向读取不能推迟其他来源的扫描期限；允许一次保守的提前全量扫描。
            if (config_.is_scan_current)
                next_full_scan_time_ = std::min(next_full_scan_time_, entry.item.next_read_time());
            // 背压/中止后保留待处理来源，但不自行唤醒，避免无进展忙等。
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
    // 仅停止读取后调用。转交已出队记录，不归还资源，也不重新入队。
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
    // 新读端先注册同一 queue，再接管旧读端转出的 pending；成功时清空调用方凭证。
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
        return count; // 包含队内开始记录，以及已 pop 但尚未完成的 pending。
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
    // 回调上下文注册后只读，与消费游标分开；共享修改集中在独立的通知字中。
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
        bool notified = false; // 仅读端使用：全量扫描碰上通知时，该来源仍立即读取 current。
        Queue::Record pending; // 已出队；失败/停止时由此读端持有，下次先处理。
        std::size_t finish_count = 0; // 本轮固定交接记录数，包含开始记录。
        Block* current = nullptr; // 队内前缀末尾 next；空快照使用 queue.current()。
        // Service 的统计去重键，不参与读取选择，也不用于优先重试。
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
                    // 完成记录已处理，flush 通知也已消费；失败/中止时仍保留来源供重试。
                    for (auto index : selected_) queues_[index].notified = false;
                }
            }
        }
        callback_.end_read();
        // 新入队的记录属于下一轮；停止生产后，这个结果可用于收尾判定。
        for (auto index : selected_) {
            const auto& entry = queues_[index];
            result.pending |= bool(entry.pending) || entry.queue->pending_count() != 0;
        }
        return result;
    }
    // 在读取任何选中来源前固定快照；后续入队留给下一轮。
    void prepare_read() noexcept {
        for (auto index : selected_) {
            auto& entry = queues_[index];
            // 完成队列非空取本轮最后一条 next；为空取 queue.current()，不访问 Writer。
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
            // 每个来源先处理自己保存的 pending，再 pop 本轮固定前缀。
            if (!entry.pending) {
                entry.pending = entry.queue->try_pop();
                if (!entry.pending) return fail(result, ReadError::foreign_queued_range);
                popped_pending_count_.fetch_add(1, std::memory_order_relaxed);
            }
            auto* block = entry.pending.get();
            if (!block) {
                finish_pending(entry);
                result.progressed = true;
                continue; // {空,next} 只推进队列，不开始完成块回调，也不计完成块数。
            }
            const auto bytes = block->readable(); // 完成块的写入位置已经固定。
            if (!bytes.empty()) {
                // 注册 queue 时指定的来源编号，不是操作系统线程号。
                // 回调只有 Block 参数，需通过 block->source_id() 区分 Writer。
                // 此元数据仅由读取线程写入；Writer 不读取它，复用时才由池 reset。
                block->source_id_ = entry.writer_id;
                if (!began) {
                    callback_.begin_read_complete(fast_epoch_time_ns()); began = true;
                    // begin 是用户代码，可能耗时或请求停止；只在调用 begin 后补查一次。
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
            // block 可能已经被立即复用；之后只更新本地状态和统计。
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
            // 没有 current 也属于空扫描，避免该来源一直用最小等待时间轮询。
            (void)entry.item.should_read_current(SteadyClock::now(), force, 0);
            entry.notified = false;
            return true;
        }
        // 每次只读一次消费游标、一次共享发布游标；回调和确认共用局部快照。
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
        // 不调用 consume() 重读 write_ptr；仅确认传给业务的那一段。
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
        // 即使位已置位也 set：该位可能是背压重试保留的，不能抑制新的生产通知。
        notification.event->set();
    }

    const ReadConfig config_;
    Callback& callback_;
    AutoResetEvent& work_event_;
    std::vector<QueueEntry> queues_;
    std::vector<std::unique_ptr<NotificationWord>> notifications_;
    std::vector<std::size_t> selected_; // 注册时预留容量；读取路径不分配内存。
    SteadyClock::time_point next_full_scan_time_{};
    bool attached_ = false;
    std::atomic<std::size_t> popped_pending_count_{0};
    std::atomic<std::uint64_t> saved_frames_{0}, saved_data_bytes_{0}, saved_finished_ranges_{0}, saved_partial_frames_{0};
    std::atomic<std::uint64_t> memory_full_events_{0}, memory_backpressured_frames_{0}, error_count_{0};
    std::atomic<ReadError> error_code_{ReadError::none};
};
} // namespace TwoBufQueue
