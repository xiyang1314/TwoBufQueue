#pragma once

#include "read_block.hpp"
#include "TwoBufQueue/src/base_components/WarmPool/warm_pool.hpp"

namespace TwoBufQueue {

// 只保存容量；创建、销毁可以由多个生产/消费线程并发调用。
class ReadBlockFactory {
public:
    ReadBlockFactory(std::size_t block_capacity, std::size_t frame_capacity)
        : block_capacity_(ReadBlock::checked_capacity(block_capacity)),
          frame_capacity_(ReadBlock::checked_frame_capacity(frame_capacity)) {}

    [[nodiscard]] ReadBlock* create() const { return new ReadBlock(block_capacity_, frame_capacity_); }
    void dispose(ReadBlock* block) const noexcept { delete block; }

private:
    const std::size_t block_capacity_;
    const std::size_t frame_capacity_;
};

using ReadBlockPool = WarmPool<ReadBlock, true, ReadBlockFactory>;

} // namespace TwoBufQueue
