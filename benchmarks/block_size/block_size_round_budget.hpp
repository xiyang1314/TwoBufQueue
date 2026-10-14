#pragma once
#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace TwoBufQueue::performance {
// Match configure_pools: preserve the reader block multiplier and use only
// the capacity actually represented by complete blocks, including free blocks.
inline std::size_t block_size_reader_capacity(std::size_t writer_block,
                                              std::size_t multiplier,
                                              std::size_t requested_bytes) {
    if (!writer_block || !multiplier || !requested_bytes ||
        writer_block > (std::numeric_limits<std::size_t>::max)()/multiplier)
        throw std::invalid_argument("invalid reader capacity reference");
    const auto block = writer_block*multiplier;
    return (std::max(requested_bytes,block)/block)*block;
}
inline std::size_t block_size_round_limit(std::size_t reader_capacity) noexcept {
    return (reader_capacity/5)*4 + ((reader_capacity%5)*4)/5;
}
} // namespace TwoBufQueue::performance
