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

// 每个物理块一个管理器：固定容量的切片释放位图、切片计数及借用代号。
// 两个参数描述分配/释放并发；任一为 true 时启用跨线程元数据同步。
// 持有统一的分配游标；调用方指定可分配终点（容量或已发布数据终点）。
// 不操作写指针、不判断 Writer 状态；关闭且全部释放后调用块的完成回调。
// 位图在构造时分配并初始化，运行中不扩容；分配锁和释放锁相互独立。
// 释放锁保护位图、释放计数快照与复用重置。
template<class Block, bool is_n_alloc, bool is_n_use>
class BasicBlockSlicesAllocator {
public:
    explicit BasicBlockSlicesAllocator(Block& block, std::size_t max_slices_per_block = 1024,
                               bool is_check_slice_release = true);
    BasicBlockSlicesAllocator(const BasicBlockSlicesAllocator&) = delete;
    BasicBlockSlicesAllocator& operator=(const BasicBlockSlicesAllocator&) = delete;

private:
    using AllocateLock = std::conditional_t<is_n_alloc, SpinLock, NoLock>;
    // 单释放者也可能与分配者、池查询并行；只在完全单线程策略中去掉释放锁。
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
    void reset(std::uint64_t sequence) noexcept; // Free 且 get 线程独占。
    void prepare_storage(); // 新块或已缩减的 Free 块在借出前确保位图存在。
    void release_storage() noexcept; // 销毁前释放位图；失效切片由池节点拒绝访问。
    [[nodiscard]] bool is_all_released() const noexcept;
    [[nodiscard]] std::size_t outstanding_count() const noexcept;

    Block& block_;
    // 先发布切片编号，再发布游标；池 acquire 游标后才检查是否全部释放。
    std::conditional_t<Block::concurrent, std::atomic<std::size_t>, PlainValue<std::size_t>> next_offset_{0};
    // 构造后不变；false 时仅忽略 release 的无效状态，不更改当前借用的元数据。
    const bool is_check_slice_release;
    const std::size_t max_slices_per_block_; // 每次借用的累计分配上限，释放不复用槽位。
    std::unique_ptr<std::uint64_t[]> released_; // 每个切片一位，1 表示已释放。
    [[no_unique_address]] mutable AllocateLock allocate_lock_; // 串行化完整分配过程，不只保护编号。
    [[no_unique_address]] mutable ReleaseLock lock_; // 保护 release/reset 及计数查询。
    std::conditional_t<is_n_alloc || is_n_use, std::atomic<std::size_t>, PlainValue<std::size_t>> allocated_count_{0}; // 串行化的分配端写入，释放端读取。
    std::size_t released_count_ = 0;
    std::uint64_t sequence_ = 0;
    bool closed_ = false;
};


inline std::size_t bitmap_words(std::size_t count) {
    if (count == 0) throw std::invalid_argument("max_slices_per_block must be positive");
    return (count >> 6) + ((count & 63) != 0); // 不使用 count + 63，避免溢出。
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
    sequence_ = 0; // 0 永不用于借出代号，无需复制派生块的业务状态。
}

template<class Block, bool is_n_alloc, bool is_n_use>
void BasicBlockSlicesAllocator<Block, is_n_alloc, is_n_use>::reset(std::uint64_t sequence) noexcept {
    std::lock_guard<ReleaseLock> lock(lock_);
    allocated_count_.store(0, std::memory_order_relaxed);
    released_count_ = 0;
    sequence_ = sequence;
    closed_ = false;
    next_offset_.store(0, std::memory_order_relaxed);
    // 保留已申请容量；每个 word 在本代首个切片发布前重置，无需清空整个数组。
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
    // 本代每个 word 的首个切片发布前清零，此时尚不存在该 word 的释放者。
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
    // 锁内认领最后完成者；调用者退出 Node 锁后再归还。
    // 无效释放不触碰新代计数或位图；默认仍按原逻辑报告错误。
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
        // 状态交接与最后一次登记均进入同一临界区，不能在锁外分别判断，
        // 否则双方都可能读到旧状态并漏掉完成通知。
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
