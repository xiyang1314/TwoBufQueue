#pragma once

#include "spsc_basic_block.hpp"
#include <atomic>
#include <span>

namespace TwoBufQueue {
template<class> class ReadService;
template<class> class SingleReadService;

enum class SpscWriteBlockState : std::uint8_t { Free, Writing, WriteFinished, LeaveWriter };

// One writer appends sequentially; one reader consumes sequentially. Consumed space is not wrapped/reused within a generation.
// Writer may release-store write_ptr_ directly or use writable/commit_write.
// Only the reader accesses the read cursor/snapshot; call to_leave_writer after external handoff completes.
class SpscWriteBlock : public SpscBasicBlock {
    static_assert(std::atomic<std::byte*>::is_always_lock_free && std::atomic<SpscWriteBlockState>::is_always_lock_free);
public:
    using State = SpscWriteBlockState;
    explicit SpscWriteBlock(std::size_t capacity) : SpscBasicBlock(capacity), write_ptr_(buf_) {}

    // Same field name/publication semantics as WriterBlock. Writer-only updates within [buf_, buf_ + capacity()].
    // Publish complete data only; advance monotonically per generation and never modify after to_write_finished.
    std::atomic<std::byte*> write_ptr_;
    std::uint64_t sequence_ = 0;

    struct ReadSnapshot {
        std::uintptr_t block_id = 0;
        std::uint64_t sequence = 0;
        std::size_t read_size = 0;
        std::size_t published_size = 0;
    };

    // Lifecycle/state queries for pool reclamation and diagnostics, independent of thread role.
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

#pragma region Producer-side calls (writer thread)
    // Fill the unpublished tail directly; never modify the already published prefix.
    [[nodiscard]] std::span<std::byte> writable() noexcept {
        if (state_.load(std::memory_order_relaxed) != State::Writing) return {};
        auto* const end = write_ptr_.load(std::memory_order_relaxed);
        return {end, capacity() - static_cast<std::size_t>(end - buf_)};
    }
    // count is newly added bytes. Publish with release semantics after writing; failure leaves the position unchanged.
    [[nodiscard]] bool commit_write(std::size_t count) noexcept {
        if (state_.load(std::memory_order_relaxed) != State::Writing) return false;
        auto* const end = write_ptr_.load(std::memory_order_relaxed);
        if (count > capacity() - static_cast<std::size_t>(end - buf_)) return false;
        write_ptr_.store(end + count, std::memory_order_release);
        return true;
    }
    // First commit_write the final tail, then mark write completion and hand off through the external queue.
    [[nodiscard]] bool to_write_finished() noexcept {
        if (state_.load(std::memory_order_relaxed) != State::Writing) return false;
        state_.store(State::WriteFinished, std::memory_order_release);
        return true;
    }

#pragma endregion

#pragma region Consumer-side calls (reader thread)
    [[nodiscard]] std::size_t read_pos() const noexcept { return read_pos_; }
    [[nodiscard]] std::size_t read_size() const noexcept { return read_pos_; }
    [[nodiscard]] std::size_t write_pos() const noexcept {
        return static_cast<std::size_t>(write_ptr_.load(std::memory_order_acquire) - buf_);
    }
    // Match WriterBlock query names; here this means consumed through the current publication point, without Slice allocation.
    [[nodiscard]] bool is_alloc_finish() const noexcept { return read_pos_ == write_pos(); }
    [[nodiscard]] ReadSnapshot read_snapshot() const noexcept {
        return {reinterpret_cast<std::uintptr_t>(this), sequence_, read_pos_, write_pos()};
    }
    // Reader only; external handoff must guarantee the writer has finished every access to this block.
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
    // Register read progress only; do not return or overwrite underlying memory.
    [[nodiscard]] bool consume(std::size_t count) noexcept {
        const auto size = write_pos();
        if (count > size - read_pos_) return false;
        read_pos_ += count;
        return true;
    }
    // Source id set by read_ctrl add_queue(queue, writer_id), not an OS thread id.
    // The reader writes this before callbacks; Writer never accesses it. Callbacks use it to distinguish sources.
    // -1 for standalone Blocks outside read_ctrl; reset restores -1 each time.
    [[nodiscard]] std::int32_t source_id() const noexcept { return source_id_; }
    // Reader only: return after the writer is done and all published bytes are consumed. Do not access the block after success.
    [[nodiscard]] bool finish() noexcept {
        // WriteFinished is not yet reclaimable: the writer's handoff record may still be unpublished.
        if (state() != State::LeaveWriter) return false;
        if (!is_alloc_finish()) return false;
        const auto saved = loan();
        if (!saved) return false;
        state_.store(State::Free, std::memory_order_release);
        return saved.recycle(); // The block may be reused immediately; do not access this after return.
    }

#pragma endregion

private:
    template<class> friend class ReadService;
    template<class> friend class SingleReadService;
    std::atomic<State> state_{State::Free};
    std::size_t read_pos_ = 0; // Reader-only access, except reset under exclusive pool ownership.
    std::int32_t source_id_ = -1;
};

} // namespace TwoBufQueue
