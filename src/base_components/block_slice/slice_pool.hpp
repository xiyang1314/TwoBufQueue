#pragma once

#include "TwoBufQueue/src/base_components/block_slice/block_resource_factory.hpp"
#include "TwoBufQueue/src/base_components/block_slice/reader_block.hpp"
#include "TwoBufQueue/src/utils/no_lock.hpp"
#include "TwoBufQueue/src/utils/spin_lock.hpp"
#include <optional>
#include <type_traits>

namespace TwoBufQueue {

// 三种场景共用的分配/换块逻辑；Block 决定释放端策略，AllocateLock 决定分配端策略。
// AllocateLock = NoLock 时只允许一个分配线程；跨线程释放能力由 Block::concurrent 决定。
// 内部池销毁前须释放所有切片；外部池必须晚于本适配器及所有切片销毁。
template<class Block, class AllocateLock>
class BasicSlicePool {
public:
    // 跨线程块的外部池统一串行化取块/归还，允许多个适配器共享同一池。
    using Factory = BlockResourceFactory<Block>;
    using Pool = WarmPool<Block, Block::concurrent, Factory>;
    using Diagnostics = BlockDiagnostics<typename Block::StorageBlock>;
    using Slice = typename Block::Slice;
    using Config = typename Block::Config;
    using ReleaseCallback = void (*)(void*) noexcept;
    BasicSlicePool(std::uint64_t block_size, std::size_t min_count, std::size_t max_count,
              Config block_config = {},
              std::uint32_t max_free_block_count = std::numeric_limits<std::uint32_t>::max())
        : owned_pool_(std::in_place, min_count, max_count, Factory{block_size, block_config},
                max_free_block_count), pool_(&*owned_pool_) {}

    // 仅借用，不接管所有权。容量、并发策略和回调由传入的池决定。
    explicit BasicSlicePool(Pool* pool) : pool_(pool) {
        if (!pool_) throw std::invalid_argument("BasicSlicePool requires a non-null WarmPool");
    }

    ~BasicSlicePool() noexcept {
        close();
        if (owned_pool_ && !pool_->all_released())
            Diagnostics::report_invalid("destroying slice pool while blocks are still in use");
        // 内部池随后析构并检查全部归还；外部池及已经交出的切片继续有效。
    }
    BasicSlicePool(const BasicSlicePool&) = delete;
    BasicSlicePool& operator=(const BasicSlicePool&) = delete;

    // 结束分配并交出最后一个块；已有切片仍有效，最后一个切片释放时归还。
    // 幂等；并发池与 allocate 使用同一把锁。其他池须由分配端串行调用。
    void close() noexcept {
        std::lock_guard<AllocateLock> lock(allocate_lock_);
        closed_ = true;
        if (auto* block = std::exchange(current_, nullptr)) {
            block->to_alloc_finished();
        }
    }
    // 作用于整个底层池；外部池所有者管理回调和上下文，适配器析构不会解除它。
    // 只在所有使用方静止时安装或解除。回调不得重入本池。
    void set_release_callback(ReleaseCallback callback, void* context) noexcept {
        pool_->set_release_callback(callback, context);
    }
    // 这些查询统计整个底层池，包括其他适配器及直接借出的块。
    [[nodiscard]] std::size_t block_count() const noexcept { return pool_->resource_count(); }
    [[nodiscard]] std::size_t in_use_count() const noexcept { return pool_->in_use_count(); }
    // 所有借出块是否已满足回收条件；任一仍可分配的 Allocating 块会使结果为 false。
    [[nodiscard]] bool all_released() const noexcept {
        std::lock_guard<AllocateLock> lock(allocate_lock_);
        return pool_->all_released();
    }

    // 空间或槽位不足时自动换块；池耗尽、申请超过单块大小时返回空句柄。
    // 不等待消费者释放；size == 0 或非法 alignment 沿用块接口的错误报告。
    [[nodiscard]] Slice allocate(std::size_t size, std::size_t alignment = 1,
        std::source_location location = std::source_location::current()) noexcept {
        if (size == 0 || alignment == 0 || alignment > Block::block_alignment ||
            (alignment & (alignment - 1)) != 0)
            Diagnostics::report_invalid("slice size/alignment is invalid", location);
        // 永远装不下的请求不能封闭当前块，避免影响后续有效的小请求。
        if (size > pool_->factory().block_size()) return {};
        // 同时串行化 current_、to_alloc_finished 和底层池的 get，不能只锁换块分支。
        std::lock_guard<AllocateLock> lock(allocate_lock_);
        if (closed_) return {};
        // 外层串行化本适配器的分配；切片释放会并发归还，底层池始终使用自身的锁。
        if (current_) {
            auto slice = current_->allow_without_lock(size, alignment, location);
            if (slice) return slice;
            auto* block = std::exchange(current_, nullptr);
            block->to_alloc_finished(location);
        }
        // 独占池由本适配器的锁保护；共享池须用自己的锁保护全部取块/归还者。
        current_ = pool_->get().get();
        if (!current_) return {};
        // 新块从偏移 0 开始，基地址按 64 字节对齐，至少有一个切片槽位。
        return current_->allow_without_lock(size, alignment, location);
    }

private:
    // 原地构造内部池，不为所有权管理额外分配堆内存；借用时保持 disengaged。
    std::optional<Pool> owned_pool_;
    Pool* const pool_;
    [[no_unique_address]] mutable AllocateLock allocate_lock_;
    Block* current_ = nullptr; // 由 allocate_lock_ 保护。
    bool closed_ = false; // 与 current_ 使用相同的串行访问约束。
};

// 三种场景独立实例化，复用上面的分配和换块逻辑。
//单线程alloc和release
class Alloc1Release1SlicePool final
    : public BasicSlicePool<Alloc1Release1ReaderBlock, NoLock> {
public:
    using Base = BasicSlicePool<Alloc1Release1ReaderBlock, NoLock>;
    using Base::Base;
};

//单线程alloc 多线程release
class Alloc1ReleaseNSlicePool final
    : public BasicSlicePool<Alloc1ReleaseNReaderBlock, NoLock> {
public:
    using Base = BasicSlicePool<Alloc1ReleaseNReaderBlock, NoLock>;
    using Base::Base;
};

//多线程alloc 多线程release
class AllocNReleaseNSlicePool final
    : public BasicSlicePool<AllocNReleaseNReaderBlock, SpinLock> {
public:
    using Base = BasicSlicePool<AllocNReleaseNReaderBlock, SpinLock>;
    using Base::Base;
};

} // namespace TwoBufQueue
