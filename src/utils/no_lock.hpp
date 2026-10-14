#pragma once

namespace TwoBufQueue {

// For caller-serialized access: one execution stream or one shared outer lock.
// Pool acquisition/scanning share this serialization requirement; the allocator independently controls concurrent slice allocation/release.
struct NoLock {
    void lock() noexcept {}
    bool try_lock() noexcept { return true; }
    void unlock() noexcept {}
};

} // namespace TwoBufQueue
