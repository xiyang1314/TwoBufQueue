#pragma once

#include "TwoBufQueue/src/base_components/WarmPool/resource.hpp"
#include <cstddef>
#include <limits>
#include <new>
#include <stdexcept>

namespace TwoBufQueue {

// 整块借出、整块归还。只管理存储和 Loan，没有 Slice 或子分配器。
// reset/finish 之前必须结束旧一代的数据访问；池必须活过全部借用。
class alignas(64) SpscBasicBlock : public DefaultResource<> {
    const std::size_t capacity_;
public:
    static constexpr std::size_t block_alignment = 64;

    explicit SpscBasicBlock(std::size_t capacity)
        : capacity_(checked_capacity(capacity)),
          buf_(static_cast<std::byte*>(::operator new(capacity_, std::align_val_t{block_alignment}))) {}
    ~SpscBasicBlock() noexcept { ::operator delete(buf_, std::align_val_t{block_alignment}); }
    SpscBasicBlock(const SpscBasicBlock&) = delete;
    SpscBasicBlock& operator=(const SpscBasicBlock&) = delete;

    // 与 WriterBlock 的缓冲区字段一致；只允许写内容，不允许更换底层存储。
    std::byte* const buf_;
    [[nodiscard]] std::byte* data() noexcept { return buf_; }
    [[nodiscard]] const std::byte* data() const noexcept { return buf_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    // 复用不清零缓冲区，调用方只读取本代已经写好并发布的字节。
    bool reset(std::uint64_t generation) noexcept { return DefaultResource<>::reset(generation); }

    static std::size_t checked_capacity(std::size_t capacity) {
        if (!capacity || capacity > static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()))
            throw std::invalid_argument("SPSC block capacity must be positive and fit ptrdiff_t");
        return capacity;
    }

};

// 创建/销毁都在池锁外；工厂只保存尺寸，不维护额外的对象容器。
template<class TBlock>
class SpscBlockFactory {
public:
    explicit SpscBlockFactory(std::size_t block_size)
        : block_size_(SpscBasicBlock::checked_capacity(block_size)) {}
    [[nodiscard]] std::size_t block_size() const noexcept { return block_size_; }
    TBlock* create() const { return new TBlock(block_size_); }
    void dispose(TBlock* block) const noexcept { delete block; }
private:
    const std::size_t block_size_;
};

} // namespace TwoBufQueue
