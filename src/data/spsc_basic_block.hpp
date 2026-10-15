#pragma once

#include "TwoBufQueue/src/base_components/warm_pool/resource.hpp"
#include <cstddef>
#include <limits>
#include <new>
#include <stdexcept>

namespace TwoBufQueue {

// Lend and return whole blocks; manages storage and Loan only, without Slice or suballocation.
// Finish old-generation data access before reset/finish; the pool must outlive every loan.
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

    // Same buffer fields as WriterBlock; contents may be written, but the underlying storage must not be replaced.
    std::byte* const buf_;
    [[nodiscard]] std::byte* data() noexcept { return buf_; }
    [[nodiscard]] const std::byte* data() const noexcept { return buf_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    // Reuse does not zero the buffer; read only bytes written and published during this generation.
    bool reset(std::uint64_t generation) noexcept { return DefaultResource<>::reset(generation); }

    static std::size_t checked_capacity(std::size_t capacity) {
        if (!capacity || capacity > static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()))
            throw std::invalid_argument("SPSC block capacity must be positive and fit ptrdiff_t");
        return capacity;
    }

};

// Create/destroy outside the pool lock; the Factory stores sizes without a separate object container.
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
