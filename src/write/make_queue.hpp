#pragma once
#include "TwoBufQueue/src/data/spsc_write_block.hpp"
#include "TwoBufQueue/src/base_components/live_handoff_queue/spsc_live_handoff_queue.hpp"
#include "TwoBufQueue/src/write/types.hpp"

namespace TwoBufQueue {
using WriterPool = WarmPool<SpscWriteBlock, true, SpscBlockFactory<SpscWriteBlock>>;
using WriterQueue = SpscLiveHandoffQueue<WriterPool>;

// Type aliases and block-lifecycle callbacks only; no new manager object.
inline constexpr WriterQueue::Callbacks writer_queue_callbacks{
    [](SpscWriteBlock& block) noexcept { if (!block.to_write_finished()) std::terminate(); },
    [](const SpscWriteBlock& block) noexcept { return block.is_alloc_finish(); },
    [](SpscWriteBlock& block) noexcept {
        if (!block.to_leave_writer() || !block.finish()) std::terminate();
    }
};

[[nodiscard]] inline WriterQueue make_writer_queue(WriterPool& pool, std::size_t capacity = 256,
                                                  bool enable_stats = false) {
    return WriterQueue(pool, capacity, writer_queue_callbacks, enable_stats);
}

// Optional source-side make_writer arguments; omitted fields retain defaults.
// Source-pool / handoff-queue / writer construction only; see TwoBufQueueOptions for reader/output configuration.
struct WriterSourceConfig {
    std::size_t block_size = 64 * 1024;      // Bytes per source block.
    std::size_t pool_min = 2;                // Minimum/initial source pool blocks.
    std::size_t pool_max = 8;                // Maximum source pool blocks.
    std::int32_t thread_id = 0;              // Producer source identifier used by readers to distinguish inputs.
    std::size_t queue_capacity = 256;        // Source-block handoff queue capacity.
    WriterBackpressureConfig backpressure{}; // Backpressure policy; disabled by default.
};

// OwnedWriter's first base owns the source pool and handoff queue;
// construct it before WriterT and destroy it afterward so the pool/queue outlive the writer.
struct WriterSourceStorage {
    WriterPool pool;
    WriterQueue queue;
    explicit WriterSourceStorage(const WriterSourceConfig& config)
        : pool(config.pool_min, config.pool_max, SpscBlockFactory<SpscWriteBlock>{config.block_size}),
          queue(make_writer_queue(pool, config.queue_capacity)) {}
};

// Self-owning writer: publicly derives from WriterT and can be passed directly to TwoBufQueue / ReadWorker;
// Hide pool/queue in private bases. WriterT needs to be complete only when initializing its base,
// so this header does not depend on writer.hpp / seq_writer.hpp.
template <class TWriter>
struct OwnedWriter : private WriterSourceStorage, public TWriter {
    explicit OwnedWriter(const WriterSourceConfig& config = {})
        : WriterSourceStorage(config),
          TWriter(&static_cast<WriterSourceStorage&>(*this).queue, config.thread_id,
                  config.backpressure) {}
};

// Construct a source pool, handoff queue and WriterT (ordinary Writer or SeqWriter), returning a ready-to-use writer.
// WriterSourceConfig contains all optional arguments.
// Usage:
//   auto writer = make_writer<Writer<>>();                     // All defaults.
//   auto writer = make_writer<SeqWriter<std::uint32_t>>();     // Writer with sequence numbers.
//   auto writer = make_writer<Writer<>>({.block_size = 4096}); // Override only the needed fields.
//   TwoBufQueue queue(callback, writer);                      // Pass the writer directly to the queue.
template <class TWriter>
[[nodiscard]] OwnedWriter<TWriter> make_writer(const WriterSourceConfig& config = {}) {
    return OwnedWriter<TWriter>(config);
}
} // namespace TwoBufQueue
