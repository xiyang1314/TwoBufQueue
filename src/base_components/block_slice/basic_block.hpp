#pragma once

#include "TwoBufQueue/src/base_components/block_slice/block_slices_allocator.hpp"
#include <stdexcept>
#include <cstring>
#include "TwoBufQueue/src/base_components/warm_pool/resource.hpp"

namespace TwoBufQueue {
struct BlockConfig {
    bool is_check_slice_release = true;
    std::size_t max_slices_per_block = 1024;
};

template<class TBlock> class BlockResourceFactory;

// Shared storage, pool identity and slice management; derived classes implement business state and recycling rules.
// false/false requires one execution stream or complete external serialization.
template<bool TIsNAlloc = false, bool TIsNUse = true>
class alignas(64) BasicBlock : public DefaultResource<TIsNAlloc || TIsNUse> {
public:
    using StorageBlock = BasicBlock;
    using Config = BlockConfig;
    static constexpr std::size_t block_alignment = 64;
    using Slice = BasicMemorySlice<BasicBlock>;
    static constexpr bool concurrent = TIsNAlloc || TIsNUse;
    BasicBlock(const BasicBlock&) = delete;
    BasicBlock& operator=(const BasicBlock&) = delete;
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t allocated_size() const noexcept { return slices_.next_offset(); }
    [[nodiscard]] std::size_t outstanding_count() const noexcept { return slices_.outstanding_count(); }
    [[nodiscard]] bool is_all_released() const noexcept { return slices_.is_all_released(); }

    // The Factory prepares physical storage; pool shrinking destroys it together with the Block.
    void prepare(std::size_t capacity) {
        std::unique_ptr<std::byte, AlignedDelete> data(static_cast<std::byte*>(
            ::operator new(capacity, std::align_val_t{64})));
        std::memset(data.get(), 0, capacity);
        initialize(data.get(), capacity);
        data_ = std::move(data);
    }
protected:
    using ResourceBase = DefaultResource<TIsNAlloc || TIsNUse>;
    using ResourceBase::finish; // Only the final completer may return it; never bypass slice lifetimes.
    bool finish_slices(bool force = false) noexcept { return slices_.close(force); }
    template<class TPredicate> bool finish_slices_when(TPredicate predicate) noexcept {
        return slices_.close_when(predicate);
    }
    explicit BasicBlock(Config config)
        : slices_(*this, config.max_slices_per_block, config.is_check_slice_release) {}
    static void validate_config(Config config) {
        if (config.max_slices_per_block == 0)
            throw std::invalid_argument("max_slices_per_block must be positive");
    }
    void initialize(std::byte* data, std::size_t capacity) {
        slices_.prepare_storage();
        buf_ = data;
        capacity_ = capacity;

    }
    // The pool has confirmed all protected operations finished; storage may be released.
    void release_storage() noexcept {
        slices_.release_storage();
        buf_ = nullptr;
        capacity_ = 0;
    }
    bool reset(std::uint64_t sequence) noexcept { slices_.reset(sequence); return ResourceBase::reset(sequence); }
    // Invalidate this generation's slices without changing business state or calling the pool. Generation zero means no valid loan.
    void invalidate_slices() noexcept { slices_.invalidate(); }
    [[nodiscard]] Slice allocate_slice(std::size_t size, std::size_t alignment, std::size_t limit,
        std::source_location location) noexcept {
        return slices_.allocate(size, alignment, limit, location);
    }
    [[nodiscard]] Slice allocate_slice_without_lock(std::size_t size, std::size_t alignment,
        std::size_t limit, std::source_location location) noexcept {
        return slices_.allow_without_lock(size, alignment, limit, location);
    }
    // Protect the derived class's entire allocation transaction; NoLock disappears for a single allocator.
    template<class TFunction> decltype(auto) with_allocation_lock(TFunction&& function) const noexcept {
        std::lock_guard guard(slices_.allocate_lock_);
        return std::forward<TFunction>(function)();
    }
    [[noreturn]] void report_error(const char* reason, std::source_location location) const noexcept {
        BlockDiagnostics<BasicBlock>::report_error(reason, this, location);
    }
    std::byte* buf_ = nullptr;

private:
    template<class TBlock> friend class BlockResourceFactory;
    friend struct BlockDiagnostics<BasicBlock>;
    template<class TBlock, bool, bool> friend class BasicBlockSlicesAllocator;
    template<class TBlock> friend class BasicMemorySlice;
    friend class BlockSlicesAllocatorForTest;
    bool release_slice(std::size_t index, std::uint64_t sequence, std::source_location location) noexcept {
        return slices_.release(index, sequence, location);
    }
    [[noreturn]] void report_inactive_slice(std::source_location location) const noexcept {
        BlockDiagnostics<BasicBlock>::report_error("slice belongs to an inactive block", this, location, true);
    }
    struct AlignedDelete {
        void operator()(std::byte* ptr) const noexcept { ::operator delete(ptr, std::align_val_t{64}); }
    };
    std::unique_ptr<std::byte, AlignedDelete> data_;
protected:
    void (*on_finished_)(BasicBlock*) noexcept = nullptr;
    void (*describe_error_)(const BasicBlock*, bool, typename BlockDiagnostics<BasicBlock>::ErrorInfo&) noexcept = nullptr;
private:
    std::size_t capacity_ = 0;
    BasicBlockSlicesAllocator<BasicBlock, TIsNAlloc, TIsNUse> slices_;
};
} // namespace TwoBufQueue
