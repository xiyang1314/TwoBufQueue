#pragma once
#include <TwoBufQueue.hpp>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace example {
inline void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

// A frame contains multiple [uint16 little-endian length][payload] records.
// Payloads may be unaligned: use memcpy, never reinterpret_cast<Record*>.
template<class Visitor>
bool messages(const TwoBufQueue::DataBatch& batch, Visitor&& visit) noexcept {
    for (const auto& frame : batch.frames) {
        const auto* p = frame.data;
        auto remaining = frame.size;
        while (remaining) {
            if (remaining < 2) return false;
            const auto length = std::to_integer<unsigned>(p[0]) |
                                (std::to_integer<unsigned>(p[1]) << 8);
            p += 2;
            remaining -= 2;
            if (length > remaining || !visit(frame.thread_id, p, length)) return false;
            p += length;
            remaining -= length;
        }
    }
    return true;
}

template<class Visitor>
bool messages(const TwoBufQueue::SpscWriteBlock* block, const std::byte* p,
              std::size_t remaining, Visitor&& visit) noexcept {
    while (remaining) {
        if (remaining < 2) return false;
        const auto length = std::to_integer<unsigned>(p[0]) | (std::to_integer<unsigned>(p[1]) << 8);
        p += 2; remaining -= 2;
        if (length > remaining || !visit(block->source_id(), p, length)) return false;
        p += length; remaining -= length;
    }
    return true;
}

struct Record { std::uint64_t producer; std::uint64_t sequence; };

inline TwoBufQueue::WriterBackpressureConfig blocking() {
    TwoBufQueue::WriterBackpressureConfig config;
    config.mode = TwoBufQueue::BackpressureMode::Block;
    return config; // bounded wait; write still returns false on timeout
}
}
