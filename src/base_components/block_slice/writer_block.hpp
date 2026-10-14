#pragma once
#include "TwoBufQueue/src/base_components/block_slice/basic_block.hpp"

namespace TwoBufQueue {
enum class WriterBlockState : std::uint8_t { Free, Writing, WriteFinished, LeaveWriter, ForceRecycle };

// Write/publish complete data before registering the published increment. Template arguments control slice allocation/release, not message writing.
// write_ptr_ still requires one publisher or external synchronization; false/false requires all accesses in one execution stream.
template<bool is_n_alloc = false, bool is_n_use = true>
struct alignas(64) WriterBlock : public BasicBlock<is_n_alloc, is_n_use> {
    using Base = BasicBlock<is_n_alloc, is_n_use>;
    using typename Base::Config;
    using typename Base::Slice;
    using State = WriterBlockState;
    explicit WriterBlock(Config config = {}) : Base(config) {
        this->describe_error_ = [](const typename Base::StorageBlock* storage, bool inactive,
            typename BlockDiagnostics<typename Base::StorageBlock>::ErrorInfo& info) noexcept {
            const auto* block = static_cast<const WriterBlock*>(storage);
            const auto state = block->state();
            info.actual_state = static_cast<std::uint8_t>(state);
            info.actual_state_name = state_name(state);
            if (inactive) info.reason = block->inactive_slice_reason();
        };
        this->on_finished_ = [](typename Base::StorageBlock* storage) noexcept {
            auto* block = static_cast<WriterBlock*>(storage);
            block->state_.store(State::Free, std::memory_order_release);
        };
    }
    using Base::validate_config;
    using Base::buf_;

    struct ReadSnapshot {
        std::uintptr_t block_id = 0;
        std::uint64_t sequence = 0;
        std::size_t read_size = 0;
        std::size_t published_size = 0;
    };
    [[nodiscard]] State state() const noexcept { return state_.load(std::memory_order_acquire); }
    [[nodiscard]] bool is_free() const noexcept { return state() == State::Free; }
    [[nodiscard]] static const char* state_name(State value) noexcept {
        switch (value) {
        case State::Free: return "Free";
        case State::Writing: return "Writing";
        case State::WriteFinished: return "WriteFinished";
        case State::LeaveWriter: return "LeaveWriter";
        case State::ForceRecycle: return "ForceRecycle";
        }
        return "Invalid";
    }
    [[nodiscard]] ReadSnapshot read_snapshot() const noexcept {
        return this->with_allocation_lock([&] { return read_snapshot_without_lock(); });
    }
    [[nodiscard]] std::size_t read_pos() const noexcept { return this->allocated_size(); }
    [[nodiscard]] std::size_t read_size() const noexcept { return read_pos(); }
    [[nodiscard]] std::size_t write_pos() const noexcept {
        return buf_ ? static_cast<std::size_t>(write_ptr_.load(std::memory_order_acquire) - buf_) : 0;
    }
    [[nodiscard]] bool is_alloc_finish() const noexcept { return read_pos() == write_pos(); }

    [[nodiscard]] Slice allocate_read_slice() noexcept {
        ReadSnapshot snapshot;
        return allocate_read_slice(snapshot);
    }
    void to_write_finished(std::source_location location = std::source_location::current()) noexcept {
        require_state(State::Writing, "expected Writing block", location);
        state_.store(State::WriteFinished, std::memory_order_release);
    }
    void to_leave_writer(std::source_location location = std::source_location::current()) noexcept {
        const auto loan = this->loan();
        bool finished = false;
        {
            std::lock_guard guard(loan);
            if (!loan.is_valid()) BlockDiagnostics<typename Base::StorageBlock>::report_invalid("expected WriteFinished block", location);
            require_state(State::WriteFinished, "expected WriteFinished block", location);
            state_.store(State::LeaveWriter, std::memory_order_release);
            finished = try_finish();
        }
        if (finished) (void)loan.recycle();
    }
    // Stop this generation's writing, slice allocation and accesses first; the call returns the block immediately.
    void force_recycle(std::source_location location = std::source_location::current()) noexcept {
        const auto loan = this->loan();
        bool finished = false;
        {
            std::lock_guard guard(loan);
            if (!loan.is_valid()) BlockDiagnostics<typename Base::StorageBlock>::report_invalid("force_recycle requires LeaveWriter", location);
            require_state(State::LeaveWriter, "force_recycle requires LeaveWriter", location);
            state_.store(State::ForceRecycle, std::memory_order_release);
            finished = this->finish_slices(true);
        }
        if (finished) (void)loan.recycle();
    }
    // Publish complete data with release semantics; growth is forbidden after WriteFinished.
    std::conditional_t<Base::concurrent, std::atomic<std::byte*>, PlainValue<std::byte*>> write_ptr_{nullptr};
    std::uint64_t sequence_ = 0;

private:
    [[nodiscard]] ReadSnapshot read_snapshot_without_lock() const noexcept {
        const auto current = state();
        if (current != State::Writing && current != State::WriteFinished && current != State::LeaveWriter)
            this->report_error("expected Writing, WriteFinished or LeaveWriter range", std::source_location::current());
        const auto begin = reinterpret_cast<std::uintptr_t>(buf_);
        const auto end = reinterpret_cast<std::uintptr_t>(write_ptr_.load(std::memory_order_acquire));
        const auto read = read_size();
        if (end < begin || end - begin > this->capacity() || read > end - begin)
            this->report_error("source cursors must satisfy read <= published <= capacity", std::source_location::current());
        return {reinterpret_cast<std::uintptr_t>(this), sequence_, read, end - begin};
    }
    [[nodiscard]] Slice allocate_read_slice(ReadSnapshot& snapshot) noexcept {
        const auto loan = this->loan();
        Slice result;
        bool finished = false;
        {
            std::lock_guard guard(loan);
            if (!loan.is_valid()) BlockDiagnostics<typename Base::StorageBlock>::report_invalid(
                "expected Writing, WriteFinished or LeaveWriter range", std::source_location::current());
            // Lock both the snapshot and registration, not only the allocator's final counter update.
            result = this->with_allocation_lock([&] {
                snapshot = read_snapshot_without_lock();
                Slice slice;
                if (snapshot.read_size != snapshot.published_size) {
                    slice = this->allocate_slice_without_lock(snapshot.published_size - snapshot.read_size, 1,
                        snapshot.published_size, std::source_location::current());
                    if (!slice) this->report_error("source slice capacity exhausted", std::source_location::current());
                }
                return slice;
            });
            finished = try_finish();
        }
        if (finished) (void)loan.recycle();
        return result;
    }
    bool try_finish() noexcept {
        return this->finish_slices_when([this] { return state() == State::LeaveWriter && is_alloc_finish(); });
    }
    void require_state(State expected, const char* reason, std::source_location location) const noexcept {
        if (state() != expected) this->report_error(reason, location);
    }
public:
    bool reset(std::uint64_t sequence) noexcept {
        require_state(State::Free, "reset requires a Free block", std::source_location::current());
        if (!Base::reset(sequence)) return false;
        write_ptr_.store(buf_, std::memory_order_relaxed);
        sequence_ = sequence;
        state_.store(State::Writing, std::memory_order_release);
        return true;
    }
private:
    [[nodiscard]] const char* inactive_slice_reason() const noexcept {
        const auto current = state();
        if (current == State::Free) return "block is already released";
        if (current == State::ForceRecycle) return "slice was invalidated by force_recycle";
        return "slice belongs to an inactive block";
    }
    std::conditional_t<Base::concurrent, std::atomic<State>, PlainValue<State>> state_{State::Free};
};
} // namespace TwoBufQueue
