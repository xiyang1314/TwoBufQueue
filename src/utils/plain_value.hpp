#pragma once

#include <atomic>
#include <utility>

namespace TwoBufQueue {

// Atomic-shaped plain storage keeps the algorithm identical without atomic operations.
template<class T> class PlainValue {
public:
    constexpr PlainValue(T value = {}) noexcept : value_(value) {}
    T load(std::memory_order = std::memory_order_seq_cst) const noexcept { return value_; }
    void store(T value, std::memory_order = std::memory_order_seq_cst) noexcept { value_ = value; }
    T operator=(T value) noexcept { value_ = value; return value; }
    T fetch_add(T value, std::memory_order = std::memory_order_seq_cst) noexcept {
        return std::exchange(value_, value_ + value);
    }
    T fetch_sub(T value, std::memory_order = std::memory_order_seq_cst) noexcept {
        return std::exchange(value_, value_ - value);
    }
    bool compare_exchange_strong(T& expected, T desired, std::memory_order, std::memory_order) noexcept {
        if (value_ == expected) { value_ = desired; return true; }
        expected = value_; return false;
    }
private:
    T value_;
};
} // namespace TwoBufQueue
