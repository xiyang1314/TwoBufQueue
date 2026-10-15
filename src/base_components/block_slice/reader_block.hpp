#pragma once
#include "TwoBufQueue/src/base_components/block_slice/basic_block.hpp"

namespace TwoBufQueue {
enum class ReaderBlockState : std::uint8_t { Free, Allocating, AllocFinished, ForceRecycle };

// Ordinary storage block: allocate space, fill it, then publish through a business queue.
template<bool TIsNAlloc = false, bool TIsNUse = true>
class ReaderBlock : public BasicBlock<TIsNAlloc, TIsNUse> {
public:
    using Base = BasicBlock<TIsNAlloc, TIsNUse>;
    using typename Base::Config;
    using typename Base::Slice;
    using State = ReaderBlockState;
    explicit ReaderBlock(Config config = {}) : Base(config) {
        this->describe_error_ = [](const typename Base::StorageBlock* storage, bool inactive,
            typename BlockDiagnostics<typename Base::StorageBlock>::ErrorInfo& info) noexcept {
            const auto* block = static_cast<const ReaderBlock*>(storage);
            const auto state = block->state();
            info.actual_state = static_cast<std::uint8_t>(state);
            info.actual_state_name = state_name(state);
            if (inactive) info.reason = block->inactive_slice_reason();
        };
        this->on_finished_ = [](typename Base::StorageBlock* storage) noexcept {
            auto* block = static_cast<ReaderBlock*>(storage);
            block->state_.store(State::Free, std::memory_order_release);
        };
    }
    using Base::validate_config;
    [[nodiscard]] State state() const noexcept { return state_.load(std::memory_order_acquire); }
    [[nodiscard]] bool is_free() const noexcept { return state() == State::Free; }
    [[nodiscard]] static const char* state_name(State value) noexcept {
        switch (value) {
        case State::Free: return "Free";
        case State::Allocating: return "Allocating";
        case State::AllocFinished: return "AllocFinished";
        case State::ForceRecycle: return "ForceRecycle";
        }
        return "Invalid";
    }
    [[nodiscard]] Slice allocate(std::size_t size, std::size_t alignment = 1,
        std::source_location location = std::source_location::current()) noexcept {
        require_state(State::Allocating, "expected Allocating block", location);
        return this->allocate_slice(size, alignment, this->capacity(), location);
    }
    // The outer layer serializes the whole allocation; do not mix with concurrent allocate calls outside the same lock.
    [[nodiscard]] Slice allow_without_lock(std::size_t size, std::size_t alignment = 1,
        std::source_location location = std::source_location::current()) noexcept {
        require_state(State::Allocating, "expected Allocating block", location);
        return this->allocate_slice_without_lock(size, alignment, this->capacity(), location);
    }
    // Call after all allocation calls finish; existing slices may still be filled, transferred and released.
    void to_alloc_finished(std::source_location location = std::source_location::current()) noexcept {
        const auto loan = this->loan();
        bool finished = false;
        {
            std::lock_guard guard(loan);
            if (!loan.is_valid()) BlockDiagnostics<typename Base::StorageBlock>::report_invalid("expected Allocating block", location);
            require_state(State::Allocating, "expected Allocating block", location);
            state_.store(State::AllocFinished, std::memory_order_release);
            finished = this->finish_slices();
        }
        if (finished) (void)loan.recycle();
    }
    [[nodiscard]] bool is_alloc_finish() const noexcept { return state() == State::AllocFinished; }

    // Stop every access to this generation first; old slices become invalid and the block is returned immediately.
    void force_recycle(std::source_location location = std::source_location::current()) noexcept {
        const auto loan = this->loan();
        bool finished = false;
        {
            std::lock_guard guard(loan);
            if (!loan.is_valid()) BlockDiagnostics<typename Base::StorageBlock>::report_invalid("force_recycle requires AllocFinished", location);
            require_state(State::AllocFinished, "force_recycle requires AllocFinished", location);
            state_.store(State::ForceRecycle, std::memory_order_release);
            finished = this->finish_slices(true);
        }
        if (finished) (void)loan.recycle();
    }

private:
    void require_state(State expected, const char* reason, std::source_location location) const noexcept {
        if (state() != expected) this->report_error(reason, location);
    }
public:
    bool reset(std::uint64_t sequence) noexcept {
        require_state(State::Free, "reset requires a Free block", std::source_location::current());
        if (!Base::reset(sequence)) return false;
        state_.store(State::Allocating, std::memory_order_release);
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
using Alloc1Release1ReaderBlock = ReaderBlock<false, false>;
using Alloc1ReleaseNReaderBlock = ReaderBlock<false, true>;
using AllocNReleaseNReaderBlock = ReaderBlock<true, true>;
} // namespace TwoBufQueue
