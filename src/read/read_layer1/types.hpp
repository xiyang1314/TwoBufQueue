#pragma once

#include "TwoBufQueue/src/data/spsc_write_block.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace TwoBufQueue {

// Read-module error categories; a callback that cannot yet handle a full range reports backpressure, not an error.
enum class ReadError : std::uint32_t {
    none,  // No error.
    null_dependency,  // Null writer, manager or read-processing callback.
    invalid_registration,  // Invalid registration parameters, or registration while running.
    duplicate_writer,  // Duplicate writer, manager or thread identifier.
    foreign_current_range,  // Current block does not belong to the specified manager.
    registration_allocation_failed,  // Failed to create storage resources during registration.
    invalid_start_state,  // Invalid startup configuration/state.
    thread_creation_failed,  // Failed to create the reader thread.
    cpu_affinity_failed,  // Failed to set reader CPU affinity.
    foreign_queued_range,  // A completed-queue block belongs to another manager.
    range_release_failed,  // Failed to return a saved block.
    free_range,  // Saved block is null or already returned.
    invalid_range_cursors,  // Invalid block read/write position.
    collection_capacity_exceeded,  // Insufficient collection-array capacity for this round.
    invalid_read_callback_result,  // Inconsistent internal collection acknowledgement count.
    finish_timeout,  // Graceful-stop deadline exceeded; unhandled source data may remain.
};

// One round's result. pending includes backpressure, not-yet-due current and records belonging to the next round.
// After production stops, force = true and pending = false together indicate all source data was collected.
// With is_scan_current = false this covers handed-off blocks only; Writer.finish() must hand off the tail before draining.
struct ReadResult {
    bool progressed = false;
    bool pending = false;
    ReadError error = ReadError::none;
};

// The caller selects reader shutdown behavior; external storage owns publication/lifetimes of already accepted data.
enum class ReadStopMode : std::uint8_t {
    slow,  // Slow stop: collect remaining source-block data and deliver it to callbacks.
    fast,  // Fast stop: stop collecting after the current callback returns.
};

// Reader startup configuration, copied on construction; runtime reconfiguration is unsupported.
struct ReadConfig {
    // pause polls when there are no completed blocks; zero sleeps immediately.
    std::uint32_t idle_spin_count = 1'000;
    // Initial scan interval: double after consecutive empty scans and reset on data, independently of callback success.
    std::chrono::nanoseconds idle_rescan_interval = std::chrono::milliseconds{5};
    // Logical CPU for the reader; negative lets the OS schedule it.
    std::int32_t worker_cpu = -1;
    // Maximum interval after empty scans, at least the initial interval; stay at this cap once reached.
    std::chrono::nanoseconds idle_rescan_max_interval = std::chrono::milliseconds{30};
    // Positive total graceful-drain deadline; notifications and partial progress do not reset it.
    // Record finish_timeout and exit on timeout; leave unhandled source data in place.
    std::chrono::milliseconds finish_timeout = std::chrono::seconds{5};
    // Minimum ordinary wait, combined with the remaining scan delay using max; an event may wake early.
    std::chrono::milliseconds work_wait_min_interval = std::chrono::milliseconds{1};
    // Retry interval while draining remains incomplete, also bounded by the remaining finish_timeout.
    std::chrono::milliseconds finish_retry_interval = std::chrono::milliseconds{1};
    // false reads completed blocks only, even after flush; hand off current before its tail can be delivered.
    // Combined with the Writer's uint32_t maximum publication count, avoids per-record publication and counting.
    bool is_scan_current = true;
};

// Cumulative reader-statistics snapshot; excludes external-storage batch/pool statistics.
struct ReadStats {
    // Nonempty source-block increments accepted by per-block callbacks.
    std::uint64_t saved_frames = 0;
    // Total raw bytes acknowledged by callbacks; successful acceptance immediately advances that block's read cursor.
    std::uint64_t saved_data_bytes = 0;
    // Source blocks whose completion records were dequeued and resources returned.
    std::uint64_t saved_finished_ranges = 0;
    // Active-block incremental frames saved during idle scans.
    std::uint64_t saved_partial_frames = 0;
    // Reserved field; the new flow does not wait for writer stalls and always reports zero.
    std::uint64_t scan_growth_count = 0;
    // Reserved field; the new flow retains no cross-round observation snapshots and always reports zero.
    std::uint64_t sequence_reset_count = 0;
    // Service's cumulative number of per-block callbacks returning false.
    std::uint64_t memory_full_events = 0;
    // Per-writer first unhandled item, deduplicated by block address + loan generation + read position.
    std::uint64_t memory_backpressured_frames = 0;
    // Lifetime total of detected configuration, queue, storage and callback errors.
    std::uint64_t error_count = 0;
};

}  // namespace TwoBufQueue
