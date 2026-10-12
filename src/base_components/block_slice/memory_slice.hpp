#pragma once

#include "TwoBufQueue/src/utils/block_diagnostics.hpp"

#include <cstddef>
#include <cstdint>
#include <source_location>
#include <utility>
#include <mutex>
#include "TwoBufQueue/src/base_components/WarmPool/resource.hpp"

namespace TwoBufQueue {

template<class Block, bool is_n_alloc, bool is_n_use> class BasicBlockSlicesAllocator;

// 一个独立释放的切片所有权句柄，按值返回，只能移动。
// 保存池节点凭证和借出代号；Block 销毁后仍可安全拒绝旧切片的释放。
// 仅并发 Block 策略允许将不同句柄交给不同线程；同一个句柄自身不支持并发访问。
template<class Block>
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
    // 明确释放后句柄变空；再次显式 release 属于非法操作并报告位置。
    // 未显式释放的有效句柄在析构时登记释放；空句柄析构无动作。
    // 更新切片释放记录；最后一个完成者可能立即归还整块。
    void release(std::source_location location = std::source_location::current()) noexcept;

private:
    template<class B, bool, bool> friend class BasicBlockSlicesAllocator;
    BasicMemorySlice(Block* block, std::byte* data, std::size_t size,
                std::size_t index, bool check_release) noexcept;
    Block* block_ = nullptr;
    Loan<void> loan_; // 内含稳定节点的锁指针和借出代号，不拥有 pool。
    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t index_ = 0; // 本次借用期间分配的切片编号。
    bool check_release_ = true; // 对象销毁后仍能决定失效释放是报错还是忽略。
};

template<class Block>
BasicMemorySlice<Block>::BasicMemorySlice(Block* block, std::byte* data, std::size_t size,
                         std::size_t index, bool check_release) noexcept
    : block_(block), loan_(block->loan()), data_(data), size_(size), index_(index),
      check_release_(check_release) {}

template<class Block>
BasicMemorySlice<Block>::~BasicMemorySlice() noexcept {
    if (block_) release();
}

template<class Block>
BasicMemorySlice<Block>::BasicMemorySlice(BasicMemorySlice&& other) noexcept
    : block_(std::exchange(other.block_, nullptr)), loan_(std::exchange(other.loan_, {})),
      data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)),
      index_(std::exchange(other.index_, 0)),
      check_release_(other.check_release_) {}

template<class Block>
BasicMemorySlice<Block>& BasicMemorySlice<Block>::operator=(BasicMemorySlice&& other) noexcept {
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

template<class Block>
void BasicMemorySlice<Block>::release(std::source_location location) noexcept {
    if (!block_) BlockDiagnostics<Block>::report_invalid("cannot release an empty or already released slice", location);
    // 先清空句柄；最后一次释放完成后池可能立即复用，返回后不访问源块。
    auto* const block = std::exchange(block_, nullptr);
    const auto loan = std::exchange(loan_, {});
    const auto index = std::exchange(index_, 0);
    data_ = nullptr;
    size_ = 0;
    bool finished = false;
    {
        std::lock_guard guard(loan); // 直接使用 Loan 中的锁指针，不访问 Block。
        if (!loan.is_valid()) {
            if (check_release_)
                BlockDiagnostics<Block>::report_invalid("slice credential is no longer valid", location);
            return;
        }
        finished = block->release_slice(index, loan.generation(), location);
    }
    // 节点操作已结束；归还时可能清理其他旧对象，不能继续持有节点锁。
    if (finished) (void)loan.recycle();
}

template<bool is_n_alloc, bool is_n_use> class BasicBlock;
using MemorySlice = BasicMemorySlice<BasicBlock<false, true>>;

} // namespace TwoBufQueue

#include "TwoBufQueue/src/base_components/block_slice/basic_block.hpp"
