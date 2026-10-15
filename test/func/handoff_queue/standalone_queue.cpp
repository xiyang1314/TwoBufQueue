// Do not include handoff_queue.hpp or handoff_reader.hpp.
#include "TwoBufQueue/src/base_components/handoff_queue/pooled_resource_queue.hpp"
#include <cassert>
#include <iostream>

using namespace TwoBufQueue;
struct Item {
    int value = 0;
    bool reset(std::uint64_t) noexcept { value = 0; return true; }
};

int main() {
    WarmPool<Item> pool(0, 2);
    {
        ClaimQueue<Item> queue(1, true);
        auto reserved = queue.try_reserve();
        assert(reserved && reserved.reservation);
        auto old_reservation = reserved.reservation;
        assert(queue.try_reserve().status == AcquireStatus::QueueFull);
        assert(queue.cancel_reservation(reserved.reservation, AcquireStatus::PoolLimit));
        assert(!queue.cancel_reservation(old_reservation, AcquireStatus::ResourceError));

        reserved = queue.try_reserve();
        auto loan = pool.get();
        assert(loan);
        auto producer = queue.attach(reserved.reservation, loan);
        assert(producer && !reserved.reservation);
        assert(queue.send(producer));
        auto claim = queue.try_claim();
        auto old_claim = claim;
        assert(queue.cancel_claim(claim));
        claim = queue.try_claim();
        assert(!queue.begin_finish(old_claim));
        auto completion = queue.begin_finish(claim);
        auto duplicate = completion;
        queue.close();
        assert(!queue.is_drained());
        assert(pool.recycle(completion.loan()));
        assert(queue.complete_finish(completion));
        assert(!queue.complete_finish(duplicate));
        assert(queue.is_drained());
        assert(queue.stats().pool_limit == 1 && queue.stats().queue_full == 1);
    }
    {
        PooledResourceQueue queue(pool, 1, true);
        auto acquired = queue.try_acquire();
        assert(acquired);
        acquired.loan->value = 42;
        assert(queue.send(acquired.loan));
        auto received = queue.try_receive();
        assert(received && received->value == 42);
        assert(received.finish()); // finish binds the standalone queue, independently of HandoffQueue.
        assert(!queue.finish(received));
        acquired = queue.try_acquire();
        assert(acquired && queue.send(acquired.loan));
        auto claim = queue.try_claim();
        assert(claim && queue.finish(claim));
        queue.close();
        assert(queue.is_drained() && pool.all_released());
    }
    std::cout << "PASS standalone_queue\n";
}
