#pragma once

#include "TwoBufQueue/src/base_components/WarmPool/resource.hpp"

// Simulated business state for core tests; end_work/completed are not required by the pool interface.
template<bool concurrent = true>
class ExampleResource : public TwoBufQueue::DefaultResource<concurrent> {
public:
    bool reset(std::uint64_t generation) noexcept {
        if (!generation) return false;
        std::uint64_t expected = 0;
        return this->generation_.compare_exchange_strong(expected, generation,
            std::memory_order_release, std::memory_order_relaxed);
    }
    bool completed() const noexcept { return this->generation() == 0; }
    bool end_work(std::uint64_t generation) noexcept {
        if (!generation) return false;
        return this->generation_.compare_exchange_strong(generation, 0,
            std::memory_order_release, std::memory_order_relaxed);
    }
};
