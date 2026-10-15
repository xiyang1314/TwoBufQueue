// A custom WarmPool factory controls how objects are created and destroyed.
// The default factory uses new/delete. Supply a custom factory for special construction,
// such as allocating extra resources or tagging objects; the pool calls create() and dispose().

#include "TwoBufQueue/src/base_components/warm_pool/warm_pool.hpp"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
using namespace TwoBufQueue;

// An increasing id distinguishes a reused object from a newly created one.
struct Connection {
    int id = 0;
    bool reset(std::uint64_t) noexcept { return true; } // The connection needs no cleanup and can be reused directly.
};

// Custom factory: create() allocates and initializes; dispose() performs complete destruction.
// dispose must be noexcept; both create and dispose run outside the pool lock.
struct ConnectionFactory {
    int next_id = 0;
    Connection* create() {
        auto* conn = new Connection;
        conn->id = ++next_id;        // Assign a new increasing id to each new object.
        return conn;
    }
    void dispose(Connection* conn) const noexcept { delete conn; }
};

int main() try {
    //---------------1. Initialize a pool with a custom factory---------------
    // The third argument is the factory; the fourth caps free objects and reclaims excess returns.
    WarmPool<Connection, true, ConnectionFactory> pool(2, 4, ConnectionFactory{}, 2);

    //---------------2. Borrow two: the factory creates ids 1 and 2-----------
    auto a = pool.get();
    auto b = pool.get();
    if (!a || !b) throw std::runtime_error("get failed");
    if (pool.in_use_count() != 2) throw std::runtime_error("in_use_count mismatch");

    //---------------3. Return and borrow again: reuse instead of allocation--
    if (!a.finish() || !b.finish()) throw std::runtime_error("recycle failed");

    auto c = pool.get();
    // An id above 2 means a new object was created (id 3); a reused object has id 1 or 2.
    if (!c || c->id > 2) throw std::runtime_error("expected a reused object, not a new one");
    if (pool.resource_count() != 2) throw std::runtime_error("resource should not grow");

    if (!c.finish()) throw std::runtime_error("recycle failed");
    if (!pool.all_released()) throw std::runtime_error("not all released");

    std::puts("PASS: custom factory creates, pool reuses objects");
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
