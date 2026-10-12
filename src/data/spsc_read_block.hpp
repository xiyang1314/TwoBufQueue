#pragma once

#include "spsc_basic_block.hpp"
#include <atomic>
#include <span>

namespace TwoBufQueue {

// 整块交接：一个线程填 data()，publish 后只交给一个读线程使用、finish。
// 可用于读取侧保存结果；没有切片登记、消费计数或自动归还。
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
    // size 是整块有效长度，允许发布空块；成功发布必须是写端的最后一次访问。
    [[nodiscard]] bool publish(std::size_t size) noexcept {
        if (size > capacity() || published_size_.load(std::memory_order_relaxed) != unpublished) return false;
        published_size_.store(size, std::memory_order_release);
        return true;
    }
    [[nodiscard]] bool is_ready() const noexcept {
        return published_size_.load(std::memory_order_acquire) != unpublished;
    }
    // 未发布时返回空；is_ready() 可区分尚未发布与已经发布的空块。
    [[nodiscard]] std::span<const std::byte> readable() const noexcept {
        const auto size = published_size_.load(std::memory_order_acquire);
        return size == unpublished ? std::span<const std::byte>{} : std::span<const std::byte>{data(), size};
    }
    // 读端处理完成后归还；成功后 data()/readable() 返回的视图全部失效。
    [[nodiscard]] bool finish() noexcept {
        if (!is_ready()) return false;
        return SpscBasicBlock::finish();
    }
private:
    std::atomic<std::size_t> published_size_{unpublished};
};

} // namespace TwoBufQueue
