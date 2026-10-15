#pragma once

#include "read_block.hpp"
#include "TwoBufQueue/src/base_components/warm_pool/warm_pool.hpp"

namespace TwoBufQueue {

// Stores capacities only; multiple producer/consumer threads may create/destroy concurrently.
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
