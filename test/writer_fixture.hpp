#pragma once
#include "TwoBufQueue/src/write/make_queue.hpp"
#include "TwoBufQueue/src/write/types.hpp"

namespace TwoBufQueue {
// Test setup only. Tests pass queue and backpressure explicitly to the real Writer.
// Teardown drains unread test data after producer/consumer objects have stopped.
struct WriterFixture {
    WriterPool pool;
    WriterQueue queue;
    WriterBackpressureConfig backpressure;
    WriterFixture(std::size_t bytes, std::size_t min, std::size_t max,
                  WriterBackpressureConfig bp = {}, std::size_t = 0, std::size_t = 2,
                  std::size_t capacity = 256, bool stats = false)
        : pool(min, max, SpscBlockFactory<SpscWriteBlock>{bytes}),
          queue(pool, capacity, writer_queue_callbacks, stats), backpressure(bp) {}
    ~WriterFixture() {
        queue.close();
        while (auto claim = queue.try_pop()) {
            if (claim.get() && !claim->consume(claim->readable().size())) std::terminate();
            queue.finish(claim);
        }
    }
    WriterQueue& handoff_queue() { return queue; }
    bool all_released() const { return pool.all_released(); }
    std::size_t in_use_count() const { return pool.in_use_count(); }
    std::size_t block_count() const { return pool.resource_count(); }
    std::size_t max_count() const { return pool.max_count(); }
    std::size_t block_size() const { return pool.factory().block_size(); }
    bool preallocate(std::size_t n) { return pool.preallocate(n); }
};
}
