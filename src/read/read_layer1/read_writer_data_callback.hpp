#pragma once
#include "TwoBufQueue/src/data/spsc_write_block.hpp"
#include <cstddef>
#include <cstdint>

namespace TwoBufQueue {
// Example callback interface, not a base class. Supply methods with these names to ReadService<MyCallback>.
// true accepts the entire supplied range; false leaves it unhandled and immediately stops this read_once round.
// Do not retain source pointers, modify cursors or return Block; never partially process a range and then return false.
struct ReadWriterDataCallback {
    void begin_read_complete(std::int64_t /*time_ns*/) noexcept {}
    bool read_complete(SpscWriteBlock* /*block*/) noexcept { return true; }
    void end_read_complete() noexcept {}
    void begin_read_current(std::int64_t /*time_ns*/) noexcept {}
    bool read_current(SpscWriteBlock* /*block*/, const std::byte* /*read_ptr*/,
                      std::size_t /*read_size*/) noexcept { return true; }
    // Called after every read_once, including failure; this notification does not imply all work succeeded.
    void end_read() noexcept {}
};
} // namespace TwoBufQueue
