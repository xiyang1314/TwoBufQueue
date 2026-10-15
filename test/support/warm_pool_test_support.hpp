#pragma once
#include "TwoBufQueue/src/base_components/block_slice/writer_block.hpp"
#include "TwoBufQueue/src/base_components/block_slice/block_resource_factory.hpp"
#include "TwoBufQueue/src/utils/block_diagnostics.hpp"

namespace TwoBufQueue {
// Test aliases and construction helpers; tests use WarmPool directly without a wrapper.
template<class TBlock, bool TConcurrent = TBlock::concurrent>
using TestBlockPool = WarmPool<TBlock, TConcurrent || TBlock::concurrent, BlockResourceFactory<TBlock>>;
template<class TPool>
using TestDiagnostics = BlockDiagnostics<typename TPool::ResourceType::StorageBlock>;
template<class TPool>
TPool make_block_pool(std::uint64_t size, std::size_t min, std::size_t max,
    typename TPool::ResourceType::Config config = {},
    std::size_t cache = std::numeric_limits<std::size_t>::max()) {
    return TPool(min, max, BlockResourceFactory<typename TPool::ResourceType>{size, config}, cache);
}
} // namespace TwoBufQueue
