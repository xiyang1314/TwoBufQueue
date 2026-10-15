#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace TwoBufQueue {
// Optional benchmark instrumentation. All updates occur under WarmPool's
// existing lock, immediately after a successful get; no per-record sampling.
struct PoolDepthStats {
    std::uint64_t samples = 0;
    std::uint64_t in_use_sum = 0, resource_sum = 0, free_sum = 0;
    std::size_t initial_in_use = 0, initial_resources = 0;
    std::size_t max_in_use = 0, max_resources = 0;

    void begin(std::size_t in_use, std::size_t resources) noexcept {
        *this = {};
        initial_in_use = max_in_use = in_use;
        initial_resources = max_resources = resources;
    }
    void record(std::size_t in_use, std::size_t resources, std::size_t free) noexcept {
        ++samples;
        in_use_sum += in_use;
        resource_sum += resources;
        free_sum += free;
        max_in_use = std::max(max_in_use, in_use);
        max_resources = std::max(max_resources, resources);
    }
};
} // namespace TwoBufQueue
