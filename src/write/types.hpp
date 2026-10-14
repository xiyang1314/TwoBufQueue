#pragma once

#include "TwoBufQueue/src/data/spsc_write_block.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <source_location>

namespace TwoBufQueue {

// Session writes never switch blocks implicitly; callers handle insufficient space with get_next_session in an outer loop.
enum class SessionWriteResult : std::uint8_t {
    success,
    insufficient_space,
    invalid_argument,
    exceeds_block_capacity, // The whole record, including length/sequence prefixes, exceeds block capacity.
    error,
};


// Pool-exhaustion policy, used only on the cold block-switch path.
enum class BackpressureMode : std::uint8_t {
    Drop,   // Reject the current record immediately so the producer can continue.
    Block,  // Wait for the saving thread to return a block; fail this write if wait_get_timeout expires.
};

// Writer backpressure configuration, used only after failing to acquire another block.
struct WriterBackpressureConfig {
    // Drop or block when the pool is exhausted.
    BackpressureMode mode = BackpressureMode::Drop;
    // Number of pause + get attempts before timed retries in Block mode.
    std::uint32_t spin_count = 1'000;
    // Total acquisition deadline including spinning/sleeping; positive and representable by the clock, validated in Writer construction.
    std::chrono::milliseconds wait_get_timeout = std::chrono::seconds{5};
};

// Writer backpressure snapshot; update counters only on exceptional pressure-related paths.
struct WriterBackpressureStats {
    // Initial next-block acquisition failures that begin backpressure handling.
    std::uint64_t pressure_events = 0;
    // Total records not written because the pool was exhausted.
    std::uint64_t dropped_records = 0;
    // Number of entries into the actual Block wait path.
    std::uint64_t blocked_events = 0;
    // Accumulated Block wait counter; measured in TSC ticks on x86/x64.
    std::uint64_t blocked_ticks = 0;
    // Longest single Block wait; uses the same units as blocked_ticks.
    std::uint64_t max_blocked_ticks = 0;
};

}  // namespace TwoBufQueue
