// WarmPool: a pool of reusable object resources.
// Reuse objects to avoid frequent new/delete calls; reset each object to a clean state before lending it.
// Returning an object makes it available for another loan. Useful for repeatedly borrowed messages, connections and similar resources.

#include "TwoBufQueue/src/base_components/WarmPool/warm_pool.hpp"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
using namespace TwoBufQueue;

// A pooled object implements reset(generation), which restores its initial state before each loan.
// Returning false rejects this loan and schedules the object for deferred destruction.
struct Message {
    int value = 0;
    bool reset(std::uint64_t) noexcept { value = 0; return true; }
};

int main() try {
    //---------------1. Initialize: at least 2 and at most 8 objects----------
    // Construction preallocates min_count (2) objects rather than creating them lazily at runtime.
    WarmPool<Message> pool(2, 8);

    //---------------2. Borrow: get() returns a non-owning Loan---------------
    auto loan = pool.get();
    if (!loan) throw std::runtime_error("get failed");   // Empty if the pool is full or object creation fails.
    loan->value = 42;                                    // Access the object like a pointer.
    if (!pool.is_valid(loan)) throw std::runtime_error("loan should be valid");

    //---------------3. Return: finish() is equivalent to recycle()-----------
    // finish is [[nodiscard]]: check its result; true means the return succeeded.
    if (!loan.finish()) throw std::runtime_error("recycle failed");

    // Borrow again: reuse the object just returned; reset() has already cleared value to zero.
    auto again = pool.get();
    if (!again || again->value != 0) throw std::runtime_error("reset not applied on reuse");
    if (!again.finish()) throw std::runtime_error("second recycle failed");

    //---------------4. Query and preallocate--------------------------------
    if (!pool.all_released()) throw std::runtime_error("not all released");
    // Bring the pool's object count up to 4 in advance to reduce allocation work in later get() calls.
    if (!pool.preallocate(4)) throw std::runtime_error("preallocate failed");
    if (pool.resource_count() != 4) throw std::runtime_error("resource_count mismatch");

    std::puts("PASS: borrow, recycle, reuse and preallocate");
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
