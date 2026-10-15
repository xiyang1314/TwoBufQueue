#pragma once
#include "TwoBufQueue/src/base_components/warm_pool/warm_pool.hpp"

namespace TwoBufQueue {
// Stores creation parameters only; WarmPool owns and destroys objects and their storage.
template<class TBlock> class BlockResourceFactory {
public:
    explicit BlockResourceFactory(std::uint64_t size, typename TBlock::Config config = {})
        : size_(static_cast<std::size_t>(size)), config_(config) {
        if (!size || size > std::numeric_limits<std::size_t>::max())
            throw std::invalid_argument("block size must fit size_t and be nonzero");
        TBlock::validate_config(config);
    }
    [[nodiscard]] std::size_t block_size() const noexcept { return size_; }
    TBlock* create() const {
        auto* block = new TBlock(config_);
        try { block->prepare(size_); }
        catch (...) { delete block; throw; }
        return block;
    }
    void dispose(TBlock* block) const noexcept { delete block; }
private:
    const std::size_t size_;
    const typename TBlock::Config config_;
};
} // namespace TwoBufQueue
