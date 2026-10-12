#pragma once

#include "spsc_basic_block.hpp"
#include <atomic>
#include <span>

namespace TwoBufQueue {
template<class> class ReadService;
template<class> class SingleReadService;

enum class SpscWriteBlockState : std::uint8_t { Free, Writing, WriteFinished, LeaveWriter };

// 一个写线程顺序追加，一个读线程顺序消费；本代已消费空间不循环使用。
// Writer 可直接 release 发布 write_ptr_，也可使用 writable/commit_write。
// 仅读端访问读取游标和快照；完成外部交接后调用 to_leave_writer。
class SpscWriteBlock : public SpscBasicBlock {
    static_assert(std::atomic<std::byte*>::is_always_lock_free && std::atomic<SpscWriteBlockState>::is_always_lock_free);
public:
    using State = SpscWriteBlockState;
    explicit SpscWriteBlock(std::size_t capacity) : SpscBasicBlock(capacity), write_ptr_(buf_) {}

    // 字段名和发布语义与 WriterBlock 一致。仅写端更新，范围为 [buf_, buf_ + capacity()]。
    // 只发布完整数据；每代单调前进，to_write_finished 后不再修改。
    std::atomic<std::byte*> write_ptr_;
    std::uint64_t sequence_ = 0;

    struct ReadSnapshot {
        std::uintptr_t block_id = 0;
        std::uint64_t sequence = 0;
        std::size_t read_size = 0;
        std::size_t published_size = 0;
    };

    // ── 生命周期与状态查询：池回收 / 诊断，不区分线程角色 ──
    bool reset(std::uint64_t generation) noexcept {
        if (!is_free()) return false;
        if (!SpscBasicBlock::reset(generation)) return false;
        write_ptr_.store(buf_, std::memory_order_relaxed);
        sequence_ = generation;
        read_pos_ = 0;
        source_id_ = -1;
        state_.store(State::Writing, std::memory_order_release);
        return true;
    }
    [[nodiscard]] State state() const noexcept { return state_.load(std::memory_order_acquire); }
    [[nodiscard]] bool is_free() const noexcept { return state() == State::Free; }
    [[nodiscard]] static const char* state_name(State value) noexcept {
        switch (value) {
        case State::Free: return "Free";
        case State::Writing: return "Writing";
        case State::WriteFinished: return "WriteFinished";
        case State::LeaveWriter: return "LeaveWriter";
        }
        return "Invalid";
    }

#pragma region product端（写线程）调用
    // 写端直接填充未发布的尾部；发布后不得再修改该前缀。
    [[nodiscard]] std::span<std::byte> writable() noexcept {
        if (state_.load(std::memory_order_relaxed) != State::Writing) return {};
        auto* const end = write_ptr_.load(std::memory_order_relaxed);
        return {end, capacity() - static_cast<std::size_t>(end - buf_)};
    }
    // count 是本次新增字节数。写好数据后 release 发布，失败不改变位置。
    [[nodiscard]] bool commit_write(std::size_t count) noexcept {
        if (state_.load(std::memory_order_relaxed) != State::Writing) return false;
        auto* const end = write_ptr_.load(std::memory_order_relaxed);
        if (count > capacity() - static_cast<std::size_t>(end - buf_)) return false;
        write_ptr_.store(end + count, std::memory_order_release);
        return true;
    }
    // 写端先 commit_write 最终尾部，再标记写完、通过外部队列交接本块。
    [[nodiscard]] bool to_write_finished() noexcept {
        if (state_.load(std::memory_order_relaxed) != State::Writing) return false;
        state_.store(State::WriteFinished, std::memory_order_release);
        return true;
    }

#pragma endregion

#pragma region client端（读线程）调用
    [[nodiscard]] std::size_t read_pos() const noexcept { return read_pos_; }
    [[nodiscard]] std::size_t read_size() const noexcept { return read_pos_; }
    [[nodiscard]] std::size_t write_pos() const noexcept {
        return static_cast<std::size_t>(write_ptr_.load(std::memory_order_acquire) - buf_);
    }
    // 对齐 WriterBlock 的查询名；此处只表示已消费到当前发布位置，不涉及 Slice 分配。
    [[nodiscard]] bool is_alloc_finish() const noexcept { return read_pos_ == write_pos(); }
    [[nodiscard]] ReadSnapshot read_snapshot() const noexcept {
        return {reinterpret_cast<std::uintptr_t>(this), sequence_, read_pos_, write_pos()};
    }
    // 仅读端调用；外部交接必须保证写端已结束对本块的全部访问。
    [[nodiscard]] bool to_leave_writer() noexcept {
        if (state() != State::WriteFinished) return false;
        state_.store(State::LeaveWriter, std::memory_order_relaxed);
        return true;
    }
    [[nodiscard]] std::span<const std::byte> readable() const noexcept {
        ReadSnapshot snapshot;
        return readable(snapshot);
    }
    [[nodiscard]] std::span<const std::byte> readable(ReadSnapshot& snapshot) const noexcept {
        snapshot = read_snapshot();
        return {buf_ + snapshot.read_size, snapshot.published_size - snapshot.read_size};
    }
    // 仅登记读进度，不归还、不覆盖底层内存。
    [[nodiscard]] bool consume(std::size_t count) noexcept {
        const auto size = write_pos();
        if (count > size - read_pos_) return false;
        read_pos_ += count;
        return true;
    }
    // 来源编号：由 read_ctrl 的 add_queue(queue, writer_id) 指定，不是 OS 线程号。
    // 读取线程在回调前写入；Writer 不访问它，回调据此区分不同来源。
    // 独立使用 Block、未接入 read_ctrl 时为 -1，每次 reset 也恢复为 -1。
    [[nodiscard]] std::int32_t source_id() const noexcept { return source_id_; }
    // 仅读端调用：写端结束且全部发布数据已消费时归还。成功后不再访问本块。
    [[nodiscard]] bool finish() noexcept {
        // WriteFinished 仍不能回收：写端的交接记录可能还没发出。
        if (state() != State::LeaveWriter) return false;
        if (!is_alloc_finish()) return false;
        const auto saved = loan();
        if (!saved) return false;
        state_.store(State::Free, std::memory_order_release);
        return saved.recycle(); // 可能立即复用本块，归还后不再访问 this。
    }

#pragma endregion

private:
    template<class> friend class ReadService;
    template<class> friend class SingleReadService;
    std::atomic<State> state_{State::Free};
    std::size_t read_pos_ = 0; // 仅读端访问；reset 时池独占。
    std::int32_t source_id_ = -1;
};

} // namespace TwoBufQueue
