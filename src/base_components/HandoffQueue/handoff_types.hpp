#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace TwoBufQueue {

enum class HandoffMode { Sync, SPSC, MPSC, SPMC, MPMC };
enum class ReaderMode { Polling, WaitSignal };
// Keep selects eligible tasks in original enqueue order; claimed tasks may run in parallel and complete out of order.
enum class CancelClaimMode { Keep, QueueTail };
enum class HandoffState { Free, Producing, Ready, Claimed, Received, Recycling, Invalid };
enum class AcquireStatus { Success, QueueFull, PoolLimit, ResourceError, Closed };

struct HandoffOptions {
    HandoffMode mode = HandoffMode::SPSC;
    std::size_t queue_capacity = 256;
    ReaderMode reader_mode = ReaderMode::WaitSignal;
    std::size_t reader_count = 1; // Ignored by Sync; SPMC/MPMC support 1..64.
    std::chrono::microseconds retry_wait{1000};
    bool enable_stats = false;
    CancelClaimMode cancel_claim_mode = CancelClaimMode::QueueTail;
};

inline CancelClaimMode validate_cancel_claim_mode(CancelClaimMode mode) {
    if (mode != CancelClaimMode::Keep && mode != CancelClaimMode::QueueTail)
        throw std::invalid_argument("invalid cancel_claim_mode");
    return mode;
}

inline HandoffOptions validate_handoff_options(HandoffOptions options) {
    (void)validate_cancel_claim_mode(options.cancel_claim_mode);
    switch (options.mode) {
    case HandoffMode::Sync: case HandoffMode::SPSC: case HandoffMode::MPSC:
    case HandoffMode::SPMC: case HandoffMode::MPMC: break;
    default: throw std::invalid_argument("invalid pipeline mode");
    }
    if (!options.queue_capacity) throw std::invalid_argument("queue_capacity must be positive");
    if (options.mode != HandoffMode::Sync) {
        if (!options.reader_count || options.reader_count > 64)
            throw std::invalid_argument("reader_count must be in [1,64]");
        if ((options.mode == HandoffMode::SPSC || options.mode == HandoffMode::MPSC) && options.reader_count != 1)
            throw std::invalid_argument("SPSC/MPSC require one reader");
        if (options.reader_mode != ReaderMode::Polling && options.reader_mode != ReaderMode::WaitSignal)
            throw std::invalid_argument("invalid reader mode");
        if (options.retry_wait <= std::chrono::microseconds::zero() || options.retry_wait > std::chrono::hours(24))
            throw std::invalid_argument("retry_wait must be in (0,24h]");
    }
    return options;
}

struct HandoffStats {
    bool counters_enabled = false;
    std::size_t queue_capacity = 0;
    std::size_t producing_count = 0, ready_count = 0, claimed_count = 0;
    std::size_t finishing_count = 0, received_count = 0;
    std::uint64_t acquire_success = 0, queue_full = 0, pool_limit = 0;
    std::uint64_t resource_error = 0, acquire_closed = 0;
};

} // namespace TwoBufQueue
