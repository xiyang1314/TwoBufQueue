#pragma once

#include "read_block.hpp"
#include "TwoBufQueue/src/data/spsc_write_block.hpp"
#include "TwoBufQueue/src/base_components/handoff_queue/handoff_types.hpp"
#include <atomic>
#include <exception>
#include <type_traits>
#include <utility>

namespace TwoBufQueue {

enum class ToReadBlockError { none, invalid_source, range_too_large };

// Ordinary template implementation of ReadWriterDataCallback; no inheritance required.
// Each read_ctrl exclusively owns one instance; multiple instances may reference one MPSC output queue.
// queue must outlive this object. Copy each source range completely; never partially acknowledge insufficient destination capacity.
template<class TQueue>
class ToReadBlock {
    static_assert(std::is_same_v<typename TQueue::Resource, ReadBlock>);
public:
    explicit ToReadBlock(TQueue& queue) noexcept : queue_(queue) {}
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
    // Submit previously accepted data even when this round later fails; source acknowledgement does not depend on downstream callback success.
    void end_read() noexcept { flush(); }

    // Retain permanent errors for this object's lifetime. Ordinary pool/queue backpressure reports its reason through AcquireStatus.
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
            flush(); // Insufficient bytes or frame slots: submit the previously acknowledged ranges first.
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
        // The new block's capacity was checked and frame_capacity is positive; failure violates the resource-reuse contract.
        if (!current_->append(writer_id, data, size, read_time_ns_)) std::terminate();
        return true;
    }
    void flush() noexcept {
        if (!current_) return;
        // Sync send may invoke business code and return/reuse the resource; clear the local ticket first.
        auto sending = std::exchange(current_, {});
        // try_acquire reserved a queue slot, so send with a valid ticket cannot fail due to full capacity.
        if (!queue_.send(sending)) std::terminate();
    }
    bool fail(ToReadBlockError error) noexcept {
        error_.store(error, std::memory_order_relaxed);
        return false;
    }

    TQueue& queue_;
    typename TQueue::ProducerLoan current_;
    std::int64_t read_time_ns_ = 0;
    std::atomic<ToReadBlockError> error_{ToReadBlockError::none};
    std::atomic<AcquireStatus> last_acquire_status_{AcquireStatus::Success};
};

template<class TQueue> ToReadBlock(TQueue&) -> ToReadBlock<TQueue>;

} // namespace TwoBufQueue
