// 故意只包含 Reader：搭配普通 int 队列，无 WarmPool/HandoffQueue。
#include "TwoBufQueue/src/base_components/HandoffQueue/handoff_reader.hpp"
#include <cassert>
#include <iostream>

using namespace TwoBufQueue;
using namespace std::chrono_literals;

class IntQueue {
public:
    using Resource = int;
    struct Claim {
        int* value = nullptr;
        explicit operator bool() const noexcept { return value != nullptr; }
        int& operator*() const noexcept { return *value; }
    };
    std::atomic<bool> ready{false}, finished{false};
    int value = 42;
    Claim try_claim() noexcept {
        return ready.exchange(false, std::memory_order_acq_rel) ? Claim{&value} : Claim{};
    }
    bool finish(Claim& claim) noexcept {
        assert(claim);
        claim = {};
        finished.store(true, std::memory_order_release);
        return true;
    }
    bool cancel_claim(Claim& claim) noexcept {
        assert(claim);
        claim = {};
        ready.store(true, std::memory_order_release);
        return true;
    }
    bool has_ready() const noexcept { return ready.load(std::memory_order_acquire); }
};

void check(HandoffMode mode) {
    IntQueue queue;
    HandoffOptions options;
    options.mode = mode;
    options.retry_wait = 200us;
    unsigned calls = 0;
    HandoffReader reader(queue, options, [&](int& value) noexcept {
        assert(value == 42);
        return ++calls == 2;
    });
    queue.ready.store(true, std::memory_order_release);
    reader.notify();
    if (mode == HandoffMode::Sync) {
        assert(!queue.finished && queue.ready && calls == 1);
        reader.process_pending();
    }
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!queue.finished.load(std::memory_order_acquire)) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    reader.stop();
    assert(calls == 2 && !reader.is_processing());
}

int main() {
    check(HandoffMode::Sync);
    check(HandoffMode::SPSC);
    std::cout << "PASS standalone_reader\n";
}
