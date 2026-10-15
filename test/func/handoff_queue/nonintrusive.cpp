#include "TwoBufQueue/src/base_components/handoff_queue/handoff_queue.hpp"
#include <cassert>
#include <iostream>
#include <thread>

using namespace TwoBufQueue;

struct Message final {
    unsigned value = 0, resets = 0;
    bool reset(std::uint64_t generation) noexcept {
        assert(generation != 0);
        ++resets;
        value = 0;
        return true;
    }
};
template<class TValue> concept HasPoolMetadata = requires(TValue& t) { t.loan(); t.state(); t.finish(); };
static_assert(PoolResource<Message> && !HasPoolMetadata<Message>);
static_assert(!std::is_base_of_v<DefaultResource<>, Message>);

struct Factory {
    std::atomic<unsigned>* disposed;
    Message* create() const { return new Message; }
    void dispose(Message* message) const noexcept { ++*disposed; delete message; }
    bool reset(Message&) = delete; // Only the resource itself implements reset.
};

int main() {
    {
        WarmPool<Message, false> pool(1, 1);
        auto first = pool.get();
        assert(first && first->resets == 1);
        first->value = 42;
        auto stale = first;
        assert(first.finish() && !stale.finish());
        auto second = pool.get();
        assert(second && second->value == 0 && second->resets == 2);
        assert(second.generation() != stale.generation());
        assert(!stale.is_valid() && second.finish());
    }
    std::atomic<unsigned> disposed{0};
    {
        WarmPool<Message, true, Factory> pool(0, 3, {&disposed}, 0);
        HandoffOptions options;
        options.mode = HandoffMode::Sync;
        options.queue_capacity = 1;
        HandoffQueue queue(pool, options, [](Message&) noexcept { return false; });

        auto first = queue.try_acquire();
        assert(first && queue.state(first.loan) == HandoffState::Producing);
        first.loan->value = 17;
        auto old_producer = first.loan;
        assert(queue.send(first.loan));
        auto received = queue.try_receive();
        assert(received && received->value == 17 && received.is_valid());
        assert(queue.state(received) == HandoffState::Received);

        // Ordinary receive has released B's sole slot, but the original resource loan remains alive.
        auto next = queue.try_acquire();
        assert(next && queue.state(old_producer) == HandoffState::Invalid);
        next.loan->value = 23;
        assert(queue.send(next.loan));
        auto claim = queue.try_claim();
        assert(claim && claim->value == 23 && queue.state(claim) == HandoffState::Claimed);
        auto stale_claim = claim;
        assert(queue.cancel_claim(claim));
        claim = queue.try_claim();
        assert(queue.state(stale_claim) == HandoffState::Invalid && !queue.finish(stale_claim));
        assert(queue.finish(claim));
        assert(queue.stats().received_count == 1 && received->value == 17);

        // Return copies of one receive Loan through different completion entries; exactly one succeeds.
        auto saved = received;
        Loan<void> erased = received;
        std::atomic<unsigned> won{0};
        std::thread a([&] { won += received.finish(); });
        std::thread b([&] { won += queue.finish(saved); });
        std::thread c([&] { won += erased.recycle(); });
        a.join(); b.join(); c.join();
        assert(won == 1 && queue.stats().received_count == 0);
        assert(!received.is_valid() && queue.state(received) == HandoffState::Invalid);

        for (int i = 0; i < 30; ++i) {
            auto item = queue.try_acquire();
            assert(item && queue.send(item.loan));
            auto loan = queue.try_receive();
            assert(loan && loan.finish());
        }
        // After object destruction/node reuse, stale tickets still touch stable nodes only.
        assert(disposed > 0 && !received.finish() && !erased.is_valid());
        assert(!queue.send(old_producer) && !queue.finish(stale_claim));

        auto raw = pool.get();
        assert(raw && !queue.finish(raw)); // An ordinary pool Loan must not impersonate a receive Loan.
        assert(raw.finish());
        queue.close();
        assert(queue.is_drained() && pool.all_released());
    }
    std::cout << "PASS nonintrusive resource reset, state, slot reuse, routed Loan and duplicate finish\n";
}
