#pragma once

#include "TwoBufQueue/src/base_components/block_slice/block_resource_factory.hpp"
#include "TwoBufQueue/src/base_components/block_slice/reader_block.hpp"
#include "TwoBufQueue/src/utils/no_lock.hpp"
#include "TwoBufQueue/src/utils/spin_lock.hpp"
#include <optional>
#include <type_traits>

namespace TwoBufQueue {

// Shared allocation/block-switch logic for three policies; Block controls release and AllocateLock controls allocation.
// AllocateLock = NoLock permits one allocator thread only; Block::concurrent determines cross-thread release support.
// Release every slice before destroying an internal pool; an external pool must outlive this adapter and all slices.
template<class TBlock, class TAllocateLock>
class BasicSlicePool {
public:
    // External concurrent-block pools serialize acquisition/return so multiple adapters may share one pool.
    using Factory = BlockResourceFactory<TBlock>;
    using Pool = WarmPool<TBlock, TBlock::concurrent, Factory>;
    using Diagnostics = BlockDiagnostics<typename TBlock::StorageBlock>;
    using Slice = typename TBlock::Slice;
    using Config = typename TBlock::Config;
    using ReleaseCallback = void (*)(void*) noexcept;
    BasicSlicePool(std::uint64_t block_size, std::size_t min_count, std::size_t max_count,
              Config block_config = {},
              std::uint32_t max_free_block_count = std::numeric_limits<std::uint32_t>::max())
        : owned_pool_(std::in_place, min_count, max_count, Factory{block_size, block_config},
                max_free_block_count), pool_(&*owned_pool_) {}

    // Borrow without taking ownership; the supplied pool controls capacity, concurrency and callbacks.
    explicit BasicSlicePool(Pool* pool) : pool_(pool) {
        if (!pool_) throw std::invalid_argument("BasicSlicePool requires a non-null WarmPool");
    }

    ~BasicSlicePool() noexcept {
        close();
        if (owned_pool_ && !pool_->all_released())
            Diagnostics::report_invalid("destroying slice pool while blocks are still in use");
        // The internal pool is destroyed next and checks all returns; external pools and handed-out slices remain valid.
    }
    BasicSlicePool(const BasicSlicePool&) = delete;
    BasicSlicePool& operator=(const BasicSlicePool&) = delete;

    // End allocation and hand off the final block; existing slices remain valid until the last release returns the block.
    // Idempotent. Concurrent pools use allocate's lock; other pools require the allocating side to serialize this call.
    void close() noexcept {
        std::lock_guard<TAllocateLock> lock(allocate_lock_);
        closed_ = true;
        if (auto* block = std::exchange(current_, nullptr)) {
            block->to_alloc_finished();
        }
    }
    // Applies to the whole underlying pool; its external owner manages callback/context. Adapter destruction does not remove it.
    // Install/remove only while all users are quiescent; the callback must not reenter this pool.
    void set_release_callback(ReleaseCallback callback, void* context) noexcept {
        pool_->set_release_callback(callback, context);
    }
    // Query the entire underlying pool, including other adapters and directly borrowed blocks.
    [[nodiscard]] std::size_t block_count() const noexcept { return pool_->resource_count(); }
    [[nodiscard]] std::size_t in_use_count() const noexcept { return pool_->in_use_count(); }
    // Whether every loaned block is reclaimable; any still-allocatable Allocating block makes this false.
    [[nodiscard]] bool all_released() const noexcept {
        std::lock_guard<TAllocateLock> lock(allocate_lock_);
        return pool_->all_released();
    }

    // Switch blocks when bytes or slice slots run out; return empty on pool exhaustion or requests larger than one block.
    // Do not wait for consumer releases; size == 0 and invalid alignment use the block interface's existing error reporting.
    [[nodiscard]] Slice allocate(std::size_t size, std::size_t alignment = 1,
        std::source_location location = std::source_location::current()) noexcept {
        if (size == 0 || alignment == 0 || alignment > TBlock::block_alignment ||
            (alignment & (alignment - 1)) != 0)
            Diagnostics::report_invalid("slice size/alignment is invalid", location);
        // Requests that can never fit must not close current, so subsequent valid small requests can still use it.
        if (size > pool_->factory().block_size()) return {};
        // Serialize current_, to_alloc_finished and pool get together, not merely the block-switch branch.
        std::lock_guard<TAllocateLock> lock(allocate_lock_);
        if (closed_) return {};
        // The outer lock serializes this adapter's allocation; concurrent slice returns always use the underlying pool's own lock.
        if (current_) {
            auto slice = current_->allow_without_lock(size, alignment, location);
            if (slice) return slice;
            auto* block = std::exchange(current_, nullptr);
            block->to_alloc_finished(location);
        }
        // This adapter's lock protects a private pool; shared pools need their own lock covering every borrower/returner.
        current_ = pool_->get().get();
        if (!current_) return {};
        // New blocks begin at offset 0, with a 64-byte-aligned base and at least one slice slot.
        return current_->allow_without_lock(size, alignment, location);
    }

private:
    // Construct the internal pool in place without an extra ownership allocation; leave it disengaged for borrowed pools.
    std::optional<Pool> owned_pool_;
    Pool* const pool_;
    [[no_unique_address]] mutable TAllocateLock allocate_lock_;
    TBlock* current_ = nullptr; // Protected by allocate_lock_.
    bool closed_ = false; // Same serialized-access constraints as current_.
};

// Separate instantiations for three policies reuse the allocation/block-switch logic above.
// Single-threaded allocation and release.
class Alloc1Release1SlicePool final
    : public BasicSlicePool<Alloc1Release1ReaderBlock, NoLock> {
public:
    using Base = BasicSlicePool<Alloc1Release1ReaderBlock, NoLock>;
    using Base::Base;
};

// Single-threaded allocation, multithreaded release.
class Alloc1ReleaseNSlicePool final
    : public BasicSlicePool<Alloc1ReleaseNReaderBlock, NoLock> {
public:
    using Base = BasicSlicePool<Alloc1ReleaseNReaderBlock, NoLock>;
    using Base::Base;
};

// Multithreaded allocation and release.
class AllocNReleaseNSlicePool final
    : public BasicSlicePool<AllocNReleaseNReaderBlock, SpinLock> {
public:
    using Base = BasicSlicePool<AllocNReleaseNReaderBlock, SpinLock>;
    using Base::Base;
};

} // namespace TwoBufQueue
