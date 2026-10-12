#pragma once

#include "read_block.hpp"
#include "TwoBufQueue/src/data/spsc_write_block.hpp"
#include "TwoBufQueue/src/base_components/HandoffQueue/handoff_types.hpp"
#include <atomic>
#include <exception>
#include <type_traits>
#include <utility>

namespace TwoBufQueue {

enum class ToReadBlockError { none, invalid_source, range_too_large };

// ReadWriterDataCallback 的普通模板实现，无需继承。
// 每个 read_ctrl 独占一个实例；多个实例可以引用同一个 MPSC 输出队列。
// queue 必须活过本对象，所有源范围按整段复制；目标不足时绝不部分确认。
template<class Queue>
class ToReadBlock {
    static_assert(std::is_same_v<typename Queue::Resource, ReadBlock>);
public:
    explicit ToReadBlock(Queue& queue) noexcept : queue_(queue) {}
    ~ToReadBlock() noexcept { flush(); }
    ToReadBlock(const ToReadBlock&) = delete;
    ToReadBlock& operator=(const ToReadBlock&) = delete;

    void begin_read_complete(std::int64_t time_ns) noexcept { read_time_ns_ = time_ns; }
    [[nodiscard]] bool read_complete(SpscWriteBlock* block) noexcept {
        if (!block) return fail(ToReadBlockError::invalid_source);
        const auto bytes = block->readable();
        return append(block->source_id(), bytes.data(), bytes.size());
    }
    void end_read_complete() noexcept {}
    void begin_read_current(std::int64_t time_ns) noexcept { read_time_ns_ = time_ns; }
    [[nodiscard]] bool read_current(SpscWriteBlock* block, const std::byte* read_ptr,
                                    std::size_t read_size) noexcept {
        if (!block) return fail(ToReadBlockError::invalid_source);
        return append(block->source_id(), read_ptr, read_size);
    }
    // 每轮失败后也提交此前已接受的数据；不把源确认与下游回调成功绑定。
    void end_read() noexcept { flush(); }

    // 永久错误保留到对象销毁；普通池/队列背压使用 AcquireStatus，区分原因。
    [[nodiscard]] ToReadBlockError error_code() const noexcept { return error_.load(std::memory_order_relaxed); }
    [[nodiscard]] AcquireStatus last_acquire_status() const noexcept {
        return last_acquire_status_.load(std::memory_order_relaxed);
    }

private:
    bool append(std::int32_t writer_id, const std::byte* data, std::size_t size) noexcept {
        if (error_code() != ToReadBlockError::none) return false;
        if (writer_id < 0 || (size && !data)) return fail(ToReadBlockError::invalid_source);
        if (!size) return true;
        if (current_) {
            if (size > current_->capacity()) return fail(ToReadBlockError::range_too_large);
            if (current_->append(writer_id, data, size, read_time_ns_)) return true;
            flush(); // 字节空间或 frame 名额不足，先提交之前已确认的范围。
        }
        auto acquired = queue_.try_acquire();
        last_acquire_status_.store(acquired.status, std::memory_order_relaxed);
        if (!acquired) return false;
        current_ = acquired.loan;
        if (size > current_->capacity()) {
            auto unused = std::exchange(current_, {});
            if (!queue_.discard(unused)) std::terminate();
            return fail(ToReadBlockError::range_too_large);
        }
        // 新块容量已检查，frame_capacity > 0；失败说明违反了资源复用契约。
        if (!current_->append(writer_id, data, size, read_time_ns_)) std::terminate();
        return true;
    }
    void flush() noexcept {
        if (!current_) return;
        // Sync 模式 send 内可能调用业务并归还/复用资源，先清空本地凭证。
        auto sending = std::exchange(current_, {});
        // try_acquire 已预留队列槽位；有效凭证的 send 不会因为容量满而失败。
        if (!queue_.send(sending)) std::terminate();
    }
    bool fail(ToReadBlockError error) noexcept {
        error_.store(error, std::memory_order_relaxed);
        return false;
    }

    Queue& queue_;
    typename Queue::ProducerLoan current_;
    std::int64_t read_time_ns_ = 0;
    std::atomic<ToReadBlockError> error_{ToReadBlockError::none};
    std::atomic<AcquireStatus> last_acquire_status_{AcquireStatus::Success};
};

template<class Queue> ToReadBlock(Queue&) -> ToReadBlock<Queue>;

} // namespace TwoBufQueue
