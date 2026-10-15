#pragma once

#include "TwoBufQueue/src/utils/block_diagnostics.hpp"

#include <cstddef>
#include <cstdint>
#include <source_location>
#include <utility>
#include <mutex>
#include "TwoBufQueue/src/base_components/warm_pool/resource.hpp"

namespace TwoBufQueue {

template<class TBlock, bool TIsNAlloc, bool TIsNUse> class BasicBlockSlicesAllocator;

// Independently releasable slice ownership handle, returned by value and move-only.
// Stores a pool-node ticket and loan generation; stale slice releases can still be rejected after Block destruction.
// Only concurrent Block policies permit different handles on different threads; one handle itself is not thread-safe.
template<class TBlock>
class BasicMemorySlice {
public:
    BasicMemorySlice() noexcept = default;
    ~BasicMemorySlice() noexcept;
    BasicMemorySlice(const BasicMemorySlice&) = delete;
    BasicMemorySlice& operator=(const BasicMemorySlice&) = delete;
    BasicMemorySlice(BasicMemorySlice&& other) noexcept;
    BasicMemorySlice& operator=(BasicMemorySlice&& other) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept { return block_ != nullptr; }
    [[nodiscard]] std::byte* data() noexcept { return data_; }
    [[nodiscard]] const std::byte* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    // Explicit release empties the handle; a second explicit release is invalid and reports its source location.
    // Destruction registers release for an unreleased valid handle; empty-handle destruction does nothing.
    // Update slice-release accounting; the final completer may immediately return the whole block.
    void release(std::source_location location = std::source_location::current()) noexcept;

private:
    template<class TOtherBlock, bool, bool> friend class BasicBlockSlicesAllocator;
    BasicMemorySlice(TBlock* block, std::byte* data, std::size_t size,
                std::size_t index, bool check_release) noexcept;
    TBlock* block_ = nullptr;
    Loan<void> loan_; // Includes the stable node's lock pointer and loan generation, without owning the pool.
    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t index_ = 0; // Slice id allocated during this loan.
    bool check_release_ = true; // Determines whether stale releases report errors or are ignored, even after object destruction.
};

template<class TBlock>
BasicMemorySlice<TBlock>::BasicMemorySlice(TBlock* block, std::byte* data, std::size_t size,
                         std::size_t index, bool check_release) noexcept
    : block_(block), loan_(block->loan()), data_(data), size_(size), index_(index),
      check_release_(check_release) {}

template<class TBlock>
BasicMemorySlice<TBlock>::~BasicMemorySlice() noexcept {
    if (block_) release();
}

template<class TBlock>
BasicMemorySlice<TBlock>::BasicMemorySlice(BasicMemorySlice&& other) noexcept
    : block_(std::exchange(other.block_, nullptr)), loan_(std::exchange(other.loan_, {})),
      data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)),
      index_(std::exchange(other.index_, 0)),
      check_release_(other.check_release_) {}

template<class TBlock>
BasicMemorySlice<TBlock>& BasicMemorySlice<TBlock>::operator=(BasicMemorySlice&& other) noexcept {
    if (this == &other) return *this;
    if (block_) release();
    block_ = std::exchange(other.block_, nullptr);
    loan_ = std::exchange(other.loan_, {});
    check_release_ = other.check_release_;
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
    index_ = std::exchange(other.index_, 0);
    return *this;
}

template<class TBlock>
void BasicMemorySlice<TBlock>::release(std::source_location location) noexcept {
    if (!block_) BlockDiagnostics<TBlock>::report_invalid("cannot release an empty or already released slice", location);
    // Clear the handle first; the final release may let the pool reuse the block immediately. Do not access the source afterward.
    auto* const block = std::exchange(block_, nullptr);
    const auto loan = std::exchange(loan_, {});
    const auto index = std::exchange(index_, 0);
    data_ = nullptr;
    size_ = 0;
    bool finished = false;
    {
        std::lock_guard guard(loan); // Use the lock pointer stored in Loan directly without touching Block.
        if (!loan.is_valid()) {
            if (check_release_)
                BlockDiagnostics<TBlock>::report_invalid("slice credential is no longer valid", location);
            return;
        }
        finished = block->release_slice(index, loan.generation(), location);
    }
    // Node operations have finished; returning may destroy other old objects, so do not retain the node lock.
    if (finished) (void)loan.recycle();
}

template<bool TIsNAlloc, bool TIsNUse> class BasicBlock;
using MemorySlice = BasicMemorySlice<BasicBlock<false, true>>;

} // namespace TwoBufQueue

#include "TwoBufQueue/src/base_components/block_slice/basic_block.hpp"
