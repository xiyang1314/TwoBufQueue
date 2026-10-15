#pragma once

#include <atomic>
#include <utility>

namespace TwoBufQueue {

// Atomic-shaped plain storage keeps the algorithm identical without atomic operations.
template<class TValue> class PlainValue {
public:
    constexpr PlainValue(TValue value = {}) noexcept : value_(value) {}
    TValue load(std::memory_order = std::memory_order_seq_cst) const noexcept { return value_; }
    void store(TValue value, std::memory_order = std::memory_order_seq_cst) noexcept { value_ = value; }
    TValue operator=(TValue value) noexcept { value_ = value; return value; }
    TValue fetch_add(TValue value, std::memory_order = std::memory_order_seq_cst) noexcept {
        return std::exchange(value_, value_ + value);
    }
    TValue fetch_sub(TValue value, std::memory_order = std::memory_order_seq_cst) noexcept {
        return std::exchange(value_, value_ - value);
    }
    bool compare_exchange_strong(TValue& expected, TValue desired, std::memory_order, std::memory_order) noexcept {
        if (value_ == expected) { value_ = desired; return true; }
        expected = value_; return false;
    }
private:
    TValue value_;
};
} // namespace TwoBufQueue
