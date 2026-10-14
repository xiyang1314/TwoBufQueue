#pragma once

#include "spsc_basic_block.hpp"
#include <atomic>
#include <span>

namespace TwoBufQueue {

// Whole-block handoff: one thread fills data(); after publish, one reader owns its use and finish.
// Suitable for storing reader-side results; no slice registration, consumed count or automatic return.
class SpscReadBlock : public SpscBasicBlock {
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
    static constexpr std::size_t unpublished = (std::numeric_limits<std::size_t>::max)();
public:
    using SpscBasicBlock::SpscBasicBlock;

    bool reset(std::uint64_t generation) noexcept {
        if (!SpscBasicBlock::reset(generation)) return false;
        published_size_.store(unpublished, std::memory_order_relaxed);
        return true;
    }
    // size is the whole valid length, including zero. A successful publish must be the writer's final access.
    [[nodiscard]] bool publish(std::size_t size) noexcept {
        if (size > capacity() || published_size_.load(std::memory_order_relaxed) != unpublished) return false;
        published_size_.store(size, std::memory_order_release);
        return true;
    }
    [[nodiscard]] bool is_ready() const noexcept {
        return published_size_.load(std::memory_order_acquire) != unpublished;
    }
    // Empty before publication; is_ready() distinguishes unpublished storage from a published empty block.
    [[nodiscard]] std::span<const std::byte> readable() const noexcept {
        const auto size = published_size_.load(std::memory_order_acquire);
        return size == unpublished ? std::span<const std::byte>{} : std::span<const std::byte>{data(), size};
    }
    // Return after reader processing; success invalidates every data()/readable() view.
    [[nodiscard]] bool finish() noexcept {
        if (!is_ready()) return false;
        return SpscBasicBlock::finish();
    }
private:
    std::atomic<std::size_t> published_size_{unpublished};
};

} // namespace TwoBufQueue
