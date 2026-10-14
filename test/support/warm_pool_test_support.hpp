#pragma once
#include "TwoBufQueue/src/base_components/block_slice/writer_block.hpp"
#include "TwoBufQueue/src/base_components/block_slice/block_resource_factory.hpp"
#include "TwoBufQueue/src/utils/block_diagnostics.hpp"

namespace TwoBufQueue {
// Test aliases and construction helpers; tests use WarmPool directly without a wrapper.
template<class Block, bool concurrent = Block::concurrent>
using TestBlockPool = WarmPool<Block, concurrent || Block::concurrent, BlockResourceFactory<Block>>;
template<class Pool>
using TestDiagnostics = BlockDiagnostics<typename Pool::ResourceType::StorageBlock>;
template<class Pool>
Pool make_block_pool(std::uint64_t size, std::size_t min, std::size_t max,
    typename Pool::ResourceType::Config config = {},
    std::size_t cache = std::numeric_limits<std::size_t>::max()) {
    return Pool(min, max, BlockResourceFactory<typename Pool::ResourceType>{size, config}, cache);
}
} // namespace TwoBufQueue
