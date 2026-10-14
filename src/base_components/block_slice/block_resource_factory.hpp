#pragma once
#include "TwoBufQueue/src/base_components/WarmPool/warm_pool.hpp"

namespace TwoBufQueue {
// Stores creation parameters only; WarmPool owns and destroys objects and their storage.
template<class Block> class BlockResourceFactory {
public:
    explicit BlockResourceFactory(std::uint64_t size, typename Block::Config config = {})
        : size_(static_cast<std::size_t>(size)), config_(config) {
        if (!size || size > std::numeric_limits<std::size_t>::max())
            throw std::invalid_argument("block size must fit size_t and be nonzero");
        Block::validate_config(config);
    }
    [[nodiscard]] std::size_t block_size() const noexcept { return size_; }
    Block* create() const {
        auto* block = new Block(config_);
        try { block->prepare(size_); }
        catch (...) { delete block; throw; }
        return block;
    }
    void dispose(Block* block) const noexcept { delete block; }
private:
    const std::size_t size_;
    const typename Block::Config config_;
};
} // namespace TwoBufQueue
