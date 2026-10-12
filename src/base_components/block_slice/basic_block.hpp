#pragma once

#include "TwoBufQueue/src/base_components/block_slice/block_slices_allocator.hpp"
#include <stdexcept>
#include <cstring>
#include "TwoBufQueue/src/base_components/WarmPool/resource.hpp"

namespace TwoBufQueue {
struct BlockConfig {
    bool is_check_slice_release = true;
    std::size_t max_slices_per_block = 1024;
};

template<class Block> class BlockResourceFactory;

// 公共存储、池归属及切片管理。业务状态和回收规则全部由派生类实现。
// false/false 要求所有访问在同一执行流中，或由外部完整串行化。
template<bool is_n_alloc = false, bool is_n_use = true>
class alignas(64) BasicBlock : public DefaultResource<is_n_alloc || is_n_use> {
public:
    using StorageBlock = BasicBlock;
    using Config = BlockConfig;
    static constexpr std::size_t block_alignment = 64;
    using Slice = BasicMemorySlice<BasicBlock>;
    static constexpr bool concurrent = is_n_alloc || is_n_use;
    BasicBlock(const BasicBlock&) = delete;
    BasicBlock& operator=(const BasicBlock&) = delete;
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t allocated_size() const noexcept { return slices_.next_offset(); }
    [[nodiscard]] std::size_t outstanding_count() const noexcept { return slices_.outstanding_count(); }
    [[nodiscard]] bool is_all_released() const noexcept { return slices_.is_all_released(); }

    // Factory 准备物理存储；池缩减时连同 Block 对象一起销毁。
    void prepare(std::size_t capacity) {
        std::unique_ptr<std::byte, AlignedDelete> data(static_cast<std::byte*>(
            ::operator new(capacity, std::align_val_t{64})));
        std::memset(data.get(), 0, capacity);
        initialize(data.get(), capacity);
        data_ = std::move(data);
    }
protected:
    using ResourceBase = DefaultResource<is_n_alloc || is_n_use>;
    using ResourceBase::finish; // 只能由最终完成者归还，不能绕过切片生命周期。
    bool finish_slices(bool force = false) noexcept { return slices_.close(force); }
    template<class Predicate> bool finish_slices_when(Predicate predicate) noexcept {
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
    // 池已确认全部受保护的操作结束，可以释放存储。
    void release_storage() noexcept {
        slices_.release_storage();
        buf_ = nullptr;
        capacity_ = 0;
    }
    bool reset(std::uint64_t sequence) noexcept { slices_.reset(sequence); return ResourceBase::reset(sequence); }
    // 使当前代切片失效，不改变业务状态、不调用池。序号 0 保留为无有效借用。
    void invalidate_slices() noexcept { slices_.invalidate(); }
    [[nodiscard]] Slice allocate_slice(std::size_t size, std::size_t alignment, std::size_t limit,
        std::source_location location) noexcept {
        return slices_.allocate(size, alignment, limit, location);
    }
    [[nodiscard]] Slice allocate_slice_without_lock(std::size_t size, std::size_t alignment,
        std::size_t limit, std::source_location location) noexcept {
        return slices_.allow_without_lock(size, alignment, limit, location);
    }
    // 覆盖派生类需要的整个分配事务；单分配者时 NoLock 被编译消除。
    template<class Function> decltype(auto) with_allocation_lock(Function&& function) const noexcept {
        std::lock_guard guard(slices_.allocate_lock_);
        return std::forward<Function>(function)();
    }
    [[noreturn]] void report_error(const char* reason, std::source_location location) const noexcept {
        BlockDiagnostics<BasicBlock>::report_error(reason, this, location);
    }
    std::byte* buf_ = nullptr;

private:
    template<class B> friend class BlockResourceFactory;
    friend struct BlockDiagnostics<BasicBlock>;
    template<class B, bool, bool> friend class BasicBlockSlicesAllocator;
    template<class B> friend class BasicMemorySlice;
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
    BasicBlockSlicesAllocator<BasicBlock, is_n_alloc, is_n_use> slices_;
};
} // namespace TwoBufQueue
