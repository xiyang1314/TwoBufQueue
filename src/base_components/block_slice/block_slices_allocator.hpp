#pragma once

#include "TwoBufQueue/src/base_components/block_slice/memory_slice.hpp"
#include "TwoBufQueue/src/utils/plain_value.hpp"
#include <type_traits>
#include "TwoBufQueue/src/utils/no_lock.hpp"
#include "TwoBufQueue/src/utils/spin_lock.hpp"

#include <memory>
#include <mutex>
#include <stdexcept>

namespace TwoBufQueue {

// One manager per physical block: fixed-capacity slice-release bitmap, slice counts and loan generation.
// The two parameters describe allocation/release concurrency; either true enables cross-thread metadata synchronization.
// Owns the common allocation cursor; callers supply the allocation limit, either capacity or the published-data end.
// Does not update write pointers or inspect Writer state; invokes the block completion callback once closed and fully released.
// Allocate and initialize the bitmap at construction, never grow it at runtime; allocation and release locks are independent.
// The release lock protects the bitmap, released-count snapshots and reuse reset.
template<class Block, bool is_n_alloc, bool is_n_use>
class BasicBlockSlicesAllocator {
public:
    explicit BasicBlockSlicesAllocator(Block& block, std::size_t max_slices_per_block = 1024,
                               bool is_check_slice_release = true);
    BasicBlockSlicesAllocator(const BasicBlockSlicesAllocator&) = delete;
    BasicBlockSlicesAllocator& operator=(const BasicBlockSlicesAllocator&) = delete;

private:
    using AllocateLock = std::conditional_t<is_n_alloc, SpinLock, NoLock>;
    // A single releaser may still overlap allocation/pool queries; omit the release lock only for the fully single-threaded policy.
    using ReleaseLock = std::conditional_t<is_n_alloc || is_n_use, SpinLock, NoLock>;
    friend Block;
    friend class BlockSlicesAllocatorForTest;
    [[nodiscard]] BasicMemorySlice<Block> allocate(std::size_t size, std::size_t alignment,
        std::size_t limit, std::source_location location) noexcept;
    [[nodiscard]] BasicMemorySlice<Block> allow_without_lock(std::size_t size, std::size_t alignment,
        std::size_t limit, std::source_location location) noexcept;
    [[nodiscard]] std::size_t next_offset() const noexcept {
        return next_offset_.load(std::memory_order_acquire);
    }
    bool release(std::size_t index, std::uint64_t sequence,
                 std::source_location location) noexcept;
    void invalidate() noexcept;
    bool close(bool force) noexcept;
    template<class Predicate> bool close_when(Predicate predicate, bool force = false) noexcept;
    void reset(std::uint64_t sequence) noexcept; // Free and exclusively owned by the get thread.
    void prepare_storage(); // Ensure a bitmap exists before lending a new or previously trimmed Free block.
    void release_storage() noexcept; // Free the bitmap before destruction; pool-node validation rejects stale slices.
    [[nodiscard]] bool is_all_released() const noexcept;
    [[nodiscard]] std::size_t outstanding_count() const noexcept;

    Block& block_;
    // Publish the slice id before its cursor; pool acquire of the cursor precedes the all-released check.
    std::conditional_t<Block::concurrent, std::atomic<std::size_t>, PlainValue<std::size_t>> next_offset_{0};
    // Immutable after construction; false suppresses invalid-release reporting without changing current-loan metadata.
    const bool is_check_slice_release;
    const std::size_t max_slices_per_block_; // Cumulative allocation limit per loan; released slots are not reused during that loan.
    std::unique_ptr<std::uint64_t[]> released_; // One bit per slice; 1 means released.
    [[no_unique_address]] mutable AllocateLock allocate_lock_; // Serialize the entire allocation, not just id assignment.
    [[no_unique_address]] mutable ReleaseLock lock_; // Protect release/reset and count queries.
    std::conditional_t<is_n_alloc || is_n_use, std::atomic<std::size_t>, PlainValue<std::size_t>> allocated_count_{0}; // Written by the serialized allocation side and read by the release side.
    std::size_t released_count_ = 0;
    std::uint64_t sequence_ = 0;
    bool closed_ = false;
};


inline std::size_t bitmap_words(std::size_t count) {
    if (count == 0) throw std::invalid_argument("max_slices_per_block must be positive");
    return (count >> 6) + ((count & 63) != 0); // Avoid count + 63, which could overflow.
}


template<class Block, bool is_n_alloc, bool is_n_use>
BasicBlockSlicesAllocator<Block, is_n_alloc, is_n_use>::BasicBlockSlicesAllocator(Block& block, std::size_t max_slices,
                                     bool check_release)
    : block_(block), is_check_slice_release(check_release),
      max_slices_per_block_(max_slices),
      released_(std::make_unique<std::uint64_t[]>(bitmap_words(max_slices))) {}

template<class Block, bool is_n_alloc, bool is_n_use>
void BasicBlockSlicesAllocator<Block, is_n_alloc, is_n_use>::prepare_storage() {
    std::lock_guard<ReleaseLock> lock(lock_);
    if (!released_) released_ = std::make_unique<std::uint64_t[]>(bitmap_words(max_slices_per_block_));
}

template<class Block, bool is_n_alloc, bool is_n_use>
void BasicBlockSlicesAllocator<Block, is_n_alloc, is_n_use>::release_storage() noexcept {
    std::lock_guard<ReleaseLock> lock(lock_);
    released_.reset();
}

template<class Block, bool is_n_alloc, bool is_n_use>
void BasicBlockSlicesAllocator<Block, is_n_alloc, is_n_use>::invalidate() noexcept {
    std::lock_guard<ReleaseLock> lock(lock_);
    sequence_ = 0; // Zero is never a loan generation; derived-block business state need not be copied.
}

template<class Block, bool is_n_alloc, bool is_n_use>
void BasicBlockSlicesAllocator<Block, is_n_alloc, is_n_use>::reset(std::uint64_t sequence) noexcept {
    std::lock_guard<ReleaseLock> lock(lock_);
    allocated_count_.store(0, std::memory_order_relaxed);
    released_count_ = 0;
    sequence_ = sequence;
    closed_ = false;
    next_offset_.store(0, std::memory_order_relaxed);
    // Retain allocated capacity; reset each word before publishing its first slice this generation, without clearing the entire array.
}

template<class Block, bool is_n_alloc, bool is_n_use>
BasicMemorySlice<Block> BasicBlockSlicesAllocator<Block, is_n_alloc, is_n_use>::allocate(
    std::size_t size, std::size_t alignment, std::size_t limit, std::source_location location) noexcept {
    std::lock_guard<AllocateLock> lock(allocate_lock_);
    return allow_without_lock(size, alignment, limit, location);
}

template<class Block, bool is_n_alloc, bool is_n_use>
BasicMemorySlice<Block> BasicBlockSlicesAllocator<Block, is_n_alloc, is_n_use>::allow_without_lock(
    std::size_t size, std::size_t alignment, std::size_t limit, std::source_location location) noexcept {
    if (size == 0 || alignment == 0 || alignment > 64 || (alignment & (alignment - 1)) != 0)
        block_.report_error("slice size/alignment is invalid", location);
    if (limit > block_.capacity_)
        block_.report_error("slice limit exceeds block capacity", location);
    const auto offset = next_offset_.load(std::memory_order_relaxed);
    const auto padding = (alignment - (offset & (alignment - 1))) & (alignment - 1);
    if (offset > limit || padding > limit - offset) return {};
    const auto start = offset + padding;
    if (size > limit - start) return {};

    const auto index = allocated_count_.load(std::memory_order_relaxed);
    if (index == max_slices_per_block_) return {};
    // Clear a word before publishing its first slice this generation, when no releaser for that word can exist yet.
    if ((index & 63) == 0) released_[index >> 6] = 0;
    allocated_count_.store(index + 1, std::memory_order_release);
    next_offset_.store(start + size, std::memory_order_release);
    return BasicMemorySlice<Block>{&block_, block_.buf_ + start, size, index, is_check_slice_release};
}

template<class Block, bool is_n_alloc, bool is_n_use>
bool BasicBlockSlicesAllocator<Block, is_n_alloc, is_n_use>::release(std::size_t index, std::uint64_t sequence,
                                 std::source_location location) noexcept {
    Block* const block = &block_;
    const char* error = nullptr;
    bool inactive = false;
    bool finished = false;
    {
        std::lock_guard<ReleaseLock> lock(lock_);
        if (sequence_ == 0) inactive = true;
        else if (sequence != sequence_ || sequence == 0) error = "slice belongs to an old block generation";
        else if (index >= allocated_count_.load(std::memory_order_acquire)) error = "slice index is out of range";
        else if ((released_[index >> 6] & (std::uint64_t{1} << (index & 63))) != 0) error = "slice is already released";
        else {
            released_[index >> 6] |= std::uint64_t{1} << (index & 63);
            ++released_count_;
            if (closed_ && released_count_ == allocated_count_.load(std::memory_order_acquire)) {
                sequence_ = 0;
                finished = true;
            }
        }
    }
    // Claim final completion under the lock; the caller returns the block after leaving the Node lock.
    // Invalid release leaves the new generation's count/bitmap untouched; default behavior still reports the error.
    if (inactive) {
        if (is_check_slice_release) block->report_inactive_slice(location);
        return false;
    }
    if (error) {
        if (is_check_slice_release) block->report_error(error, location);
        return false;
    }
    if (finished) block->on_finished_(block);
    return finished;
}

template<class Block, bool is_n_alloc, bool is_n_use>
bool BasicBlockSlicesAllocator<Block, is_n_alloc, is_n_use>::is_all_released() const noexcept {
    std::lock_guard<ReleaseLock> lock(lock_);
    return released_count_ == allocated_count_.load(std::memory_order_acquire);
}

template<class Block, bool is_n_alloc, bool is_n_use>
std::size_t BasicBlockSlicesAllocator<Block, is_n_alloc, is_n_use>::outstanding_count() const noexcept {
    std::lock_guard<ReleaseLock> lock(lock_);
    return allocated_count_.load(std::memory_order_acquire) - released_count_;
}

template<class Block, bool is_n_alloc, bool is_n_use>
bool BasicBlockSlicesAllocator<Block, is_n_alloc, is_n_use>::close(bool force) noexcept {
    return close_when([] { return true; }, force);
}

template<class Block, bool is_n_alloc, bool is_n_use>
template<class Predicate>
bool BasicBlockSlicesAllocator<Block, is_n_alloc, is_n_use>::close_when(Predicate predicate, bool force) noexcept {
    bool finished = false;
    {
        std::lock_guard<ReleaseLock> guard(lock_);
        // State handoff and final registration must share one critical section rather than checking separately outside the lock;
        // otherwise both sides could observe old state and miss completion notification.
        if (sequence_ && predicate()) {
            closed_ = true;
            if (force || released_count_ == allocated_count_.load(std::memory_order_acquire)) {
                sequence_ = 0;
                finished = true;
            }
        }
    }
    if (finished) block_.on_finished_(&block_);
    return finished;
}
} // namespace TwoBufQueue
