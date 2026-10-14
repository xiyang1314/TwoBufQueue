#include "TwoBufQueue/src/base_components/WarmPool/warm_pool.hpp"
#include "example_resource.hpp"

#include <array>
#include <barrier>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <latch>
#include <memory>
#include <string_view>
#include <unordered_set>

using namespace TwoBufQueue;
using namespace std::chrono_literals;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "CHECK failed: " #condition " at line " << __LINE__ << '\n'; std::abort(); \
} } while (false)

namespace TwoBufQueue {
struct WarmPoolTestAccess {
    template<class Pool> static void exhaust_next(Pool& pool) {
        pool.next_generation_ = std::numeric_limits<std::uint64_t>::max() - 1;
    }
};
}

struct Control {
    std::atomic<int> creates{0}, destroys{0}, recycles{0}, deletes{0};
    std::atomic<int> create_mode{0}, reset_mode{0};
    std::atomic<int> disposes{0}, allocations{0}, frees{0};
    std::function<void()> on_create, on_destroy, on_dispose, on_reset;
};

struct Payload : ExampleResource<> {
    Control* control;
    std::array<std::uint64_t, 8> data{};
    explicit Payload(Control* value) : control(value) { ++control->creates; }
    ~Payload() noexcept {
        ++control->destroys;
        if (control->on_destroy) control->on_destroy();
    }
    bool reset(std::uint64_t generation) noexcept {
        if (control->on_reset) control->on_reset();
        const auto mode = control->reset_mode.load();
        if (mode == 1) return false;
        if (mode == 2) { (void)ExampleResource<>::reset(generation); return false; } // Partial preparation failure.
        data.fill(0);
        return ExampleResource<>::reset(generation);
    }
};

template<class T>
struct Factory {
    Control* control;
    T* create() const {
        if (control->on_create) control->on_create();
        const auto mode = control->create_mode.load();
        if (mode == 1) return {};
        if (mode == 2) throw std::bad_alloc{};
        return new T(control);
    }
    void dispose(T* resource) const noexcept {
        if (control->on_dispose) control->on_dispose();
        ++control->disposes;
        delete resource;
    }
};
using Pool = WarmPool<Payload, true, Factory<Payload>>;

// Concrete type owning a large resource; only preparation/cleanup are required. The Factory recreates it after eviction.
struct Heavy : Payload {
    std::unique_ptr<int[]> storage = std::make_unique<int[]>(64);
    using Payload::Payload;
};
using HeavyPool = WarmPool<Heavy, true, Factory<Heavy>>;

template<class T> concept HasEndWork = requires(T& value) { value.end_work(std::uint64_t{1}); };
template<class T> concept HasOnRecycle = requires(T& value) { value.on_recycle(); };
template<class T> concept HasRestore = requires(T& value) { value.restore_resources(); };
template<class T> concept HasGeneration = requires(const T& value) { value.generation(); };

template<class T> void finish(const T& resource) {
    CHECK(resource != nullptr);
    const auto generation = resource.generation();
    CHECK(generation != 0);
    CHECK(resource->end_work(generation));
}

template<class P> void release(P& pool, const typename P::Loan& loan) {
    finish(loan);
    CHECK(pool.recycle(loan));
}

void protocol() {
    struct Missing {};
    struct Throwing {
        bool reset(std::uint64_t) { return true; }
        bool completed() const noexcept { return true; }
        void dispose() noexcept {}
    };
    struct WrongReturn {
        int reset(std::uint64_t) noexcept { return 1; }
        bool completed() const noexcept { return true; }
        void dispose() noexcept {}
    };
    struct MissingDispose {
        bool reset(std::uint64_t) noexcept { return true; }
        bool completed() const noexcept { return false; }
    };
    struct ThrowingDispose : DefaultResource<> { void dispose() {} };
    struct WrongDispose : DefaultResource<> { bool dispose() noexcept { return true; } };
    static_assert(PoolResource<DefaultResource<>> && PoolResource<DefaultResource<false>>);
    static_assert(PoolResource<Payload> && PoolResource<Heavy>);
    static_assert(!PoolResource<Missing> && !PoolResource<Throwing> && !PoolResource<WrongReturn>);
    static_assert(PoolResource<MissingDispose> && PoolResource<ThrowingDispose> && PoolResource<WrongDispose>);
    static_assert(!HasEndWork<DefaultResource<>> && HasGeneration<DefaultResource<>>);
    static_assert(!HasOnRecycle<DefaultResource<>> && !HasRestore<DefaultResource<>>);
    static_assert(HasEndWork<ExampleResource<>>);
}

template<bool concurrent> void resource_one() {
    ExampleResource<concurrent> resource;
    CHECK(resource.completed());
    CHECK(!resource.reset(0));
    CHECK(resource.reset(4));
    CHECK(!resource.reset(5));
    CHECK(!resource.end_work(0) && !resource.end_work(3));
    CHECK(resource.generation() == 4 && !resource.completed());
    CHECK(resource.end_work(4));
    CHECK(!resource.end_work(4)); // Independent stack object whose lifetime remains valid.
    CHECK(resource.reset(5));
    CHECK(!resource.end_work(4));
    CHECK(resource.generation() == 5);
    CHECK(resource.end_work(5));
}

void resource() {
    resource_one<false>();
    resource_one<true>();
    ExampleResource<> value;
    for (std::uint64_t generation = 1; generation <= 200; ++generation) {
        CHECK(value.reset(generation));
        std::atomic<int> successes{0};
        std::array<std::thread, 4> workers;
        for (auto& thread : workers) thread = std::thread([&] {
            if (value.end_work(generation)) ++successes;
        });
        for (auto& thread : workers) thread.join();
        CHECK(successes == 1 && value.completed());
    }
}

void ownership_checks() {
    using P = WarmPool<DefaultResource<>>;
    for (std::size_t cached : {0u, 1u}) {
        P pool(0, 1, {}, cached), other(0, 1);
        DefaultResource<> unbound;
        CHECK(!pool.owns(P::Loan{}) && !pool.owns(Loan<void>{}));
        CHECK(!pool.owns(unbound) && !pool.owns(nullptr));
        auto first = pool.get();
        auto handle = first->loan();
        CHECK(pool.owns(first) && pool.owns(handle) && pool.owns(*first) && pool.owns(first.get()));
        CHECK(!other.owns(first) && !other.owns(handle) && !other.owns(*first));
        CHECK(pool.is_valid(first) && !other.is_valid(first));
        CHECK(first->finish());
        // Below, access stable nodes and loan-time tickets only.
        CHECK(pool.resource_count() == cached);
        CHECK(pool.owns(first) && pool.owns(handle) && !pool.is_valid(first));
        auto next = pool.get();
        if (cached) CHECK(next.get() == first.get()); // Ensure the same address is reused.
        CHECK(next.generation() != first.generation());
        CHECK(pool.owns(first) && !pool.is_valid(first) && !pool.recycle(first));
        CHECK(pool.is_valid(next) && next->finish());
    }
}

void lifecycle() {
    ownership_checks();
    Control control;
    Pool pool(2, 3, {&control});
    Pool other(0, 1, {&control});
    CHECK(pool.resource_count() == 2 && pool.free_resource_count() == 2);
    CHECK(!pool.preallocate(4));
    auto a = pool.get(); auto b = pool.get(); auto c = pool.get();
    CHECK(a && b && c && a.get() != b.get() && a.get() != c.get() && b.get() != c.get());
    CHECK(pool.get() == nullptr && pool.in_use_count() == 3);
    CHECK(pool.owns(a.get()) && !pool.owns(nullptr) && !pool.all_released());
    CHECK(!pool.is_valid(Pool::Loan{}) && pool.is_valid(a) && !other.is_valid(a));
    CHECK(!pool.recycle(Pool::Loan{}) && !other.recycle(a));
    auto* old_address = b.get();
    const auto old_generation = b.generation();
    finish(b);
    CHECK(pool.is_valid(b)); // Business work ended but the loan has not returned; the ticket remains valid.
    CHECK(pool.in_use_count() == 3 && pool.free_resource_count() == 0);
    CHECK(!pool.all_released()); // Only simulated business work ends here; the resource is not returned yet.
    std::size_t notifications = 0;
    pool.set_release_callback([](void* value) noexcept { ++*static_cast<std::size_t*>(value); }, &notifications);
    CHECK(pool.recycle(b) && notifications == 1);
    CHECK(!pool.is_valid(b) && !pool.recycle(b) && notifications == 1);
    auto again = pool.get();
    CHECK(again.get() == old_address && again.generation() > old_generation);
    CHECK(!pool.is_valid(b) && pool.is_valid(again));
    CHECK(!pool.recycle(b));
    finish(again); // The new loan has completed; an old ticket must not return it either.
    CHECK(!pool.recycle(b) && pool.in_use_count() == 3);
    release(pool, a); release(pool, c);
    CHECK(pool.recycle(again) && notifications == 4);
    CHECK(pool.all_released() && pool.pending_count() == 0);
    pool.set_release_callback(nullptr, nullptr);
    for (const auto min : {std::size_t{0}, std::size_t{4}}) {
        bool rejected = false;
        try { Pool invalid(min, min == 0 ? 0 : 3, {&control}); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
    }
}

void independent() {
    struct Independent {
        bool done = true;
        bool reset(std::uint64_t) noexcept { done = false; return true; }
        bool completed() const noexcept { return done; }
        void dispose() noexcept {}
    };
    static_assert(PoolResource<Independent>);
    static_assert(!HasEndWork<Independent> && !HasOnRecycle<Independent> && !HasRestore<Independent>);
    WarmPool<Independent, false> pool(0, 1);
    auto value = pool.get();
    CHECK(value && pool.get() == nullptr);
    CHECK(pool.owns(value) && pool.is_valid(value));
    value->done = true;
    CHECK(pool.recycle(value));
    CHECK(pool.owns(value) && !pool.is_valid(value));
}

void failures() {
    Control control;
    Pool pool(0, 1, {&control});
    for (int mode : {1, 2}) {
        control.create_mode = mode;
        CHECK(pool.get() == nullptr);
        CHECK(pool.resource_count() == 0 && pool.pending_count() == 0);
    }
    control.create_mode = 0;
    for (int mode : {1, 2}) {
        control.reset_mode = mode;
        CHECK(pool.get() == nullptr);
        CHECK(pool.resource_count() == 0 && pool.in_use_count() == 0 && pool.pending_count() == 0);
    }
    CHECK(control.creates == control.destroys + 1 && pool.will_dispose_count() == 1);
    control.reset_mode = 0;
    release(pool, pool.get());
    Control bad;
    bad.create_mode = 2;
    bool rejected = false;
    try { Pool failed(1, 1, {&bad}); }
    catch (const std::bad_alloc&) { rejected = true; }
    CHECK(rejected && bad.creates == bad.destroys);
}

void factory_dispose() {
    // The Factory owns complete destruction; the pool must not delete again after dispose.
    struct CustomFactory {
        Control* control;
        Payload* create() const { return new Payload(control); }
        void dispose(Payload* resource) const noexcept {
            ++control->disposes;
            ++control->deletes;
            delete resource;
        }
    };
    Control control;
    {
        WarmPool<Payload, true, CustomFactory> pool(0, 2, {&control}, 0);
        release(pool, pool.get());
        CHECK(control.deletes == 0 && pool.will_dispose_count() == 1);
        control.reset_mode = 1;
        CHECK(pool.get() == nullptr && control.deletes == 1);
        control.reset_mode = 0;
        release(pool, pool.get());
        CHECK(control.deletes == 2);
    }
    CHECK(control.deletes == 3 && control.disposes == 3 && control.creates == control.destroys);
}

void trim_destroy() {
    Control control;
    Pool pool(2, 6, {&control}, 2);
    std::array<Pool::Loan, 6> values;
    for (auto& value : values) value = pool.get();
    auto* latest_cached = values[1].get();
    for (std::size_t i = 0; i < 5; ++i) release(pool, values[i]);
    CHECK(pool.free_resource_count() == 2 && pool.resource_count() == 3 && control.destroys == 2);
    auto hot = pool.get();
    CHECK(hot.get() == latest_cached); // Preserve LIFO reuse; exceeding the free-cache limit destroys the object being returned.
    release(pool, hot); release(pool, values[5]);
    CHECK(pool.resource_count() == 2 && pool.free_resource_count() == 2);
    CHECK(pool.preallocate(6) && pool.resource_count() == 6);
    // No whole-pool scan; explicitly warmed resources are trimmed individually as they are borrowed/returned.
    for (auto& value : values) value = pool.get();
    for (const auto& value : values) release(pool, value);
    CHECK(pool.resource_count() == 2 && pool.free_resource_count() == 2);
}

void trim_recreate() {
    Control control;
    HeavyPool pool(0, 1, {&control}, 0);
    auto value = pool.get();
    finish(value);
    CHECK(pool.recycle(value));
    CHECK(control.destroys == 0 && control.disposes == 0 && pool.resource_count() == 0);
    for (int mode : {1, 2}) {
        control.create_mode = mode;
        CHECK(pool.get() == nullptr);
        CHECK(pool.pending_count() == 0 && pool.resource_count() == 0);
    }
    control.create_mode = 0;
    auto recreated = pool.get();
    CHECK(recreated && recreated->storage && control.creates == 2);
    finish(recreated);
    CHECK(pool.recycle(recreated) && control.destroys == 1 && control.disposes == 1);
}

void slow_hooks() {
    Control control;
    HeavyPool pool(0, 1, {&control}, 0);
    auto observe = [&] {
        CHECK(pool.pending_count() == 1);
        CHECK(pool.free_resource_count() == 0);
    };
    control.on_create = observe;
    control.on_dispose = observe;
    control.on_reset = observe;
    release(pool, pool.get());
    release(pool, pool.get());
    control.on_create = {}; control.on_dispose = {}; control.on_reset = {};
    Control plain;
    Pool other(0, 1, {&plain}, 0);
    plain.on_dispose = [&] { CHECK(other.pending_count() == 1 && other.free_resource_count() == 0); };
    plain.on_destroy = [&] { CHECK(other.pending_count() == 1 && other.free_resource_count() == 0); };
    release(other, other.get());
    plain.on_dispose = {};
    plain.on_destroy = {};
}

void trim_overlap() {
    Control control;
    HeavyPool pool(3, 6, {&control}, 1);
    std::array<HeavyPool::Loan, 6> values;
    for (auto& value : values) value = pool.get();
    release(pool, values[0]);
    std::latch entered(1), resume(1);
    std::atomic<bool> first{true};
    control.on_dispose = [&] {
        if (first.exchange(false)) { entered.count_down(); resume.wait(); }
    };
    release(pool, values[1]); // First registration must not destroy itself.
    finish(values[2]);
    std::thread one([&] { CHECK(pool.recycle(values[2])); });
    entered.wait();
    CHECK(!pool.recycle(values[1])); // Safely reject while another thread is destroying this object as well.
    for (std::size_t i = 3; i < 6; ++i) release(pool, values[i]);
    CHECK(pool.free_resource_count() == 3 && control.disposes == 0);
    resume.count_down();
    one.join();
    control.on_dispose = {};
    CHECK(pool.resource_count() == 3 && pool.free_resource_count() == 3 && pool.pending_count() == 0);
    CHECK(control.disposes == 1 && pool.will_dispose_count() == 2);
}

void wait_case() {
    WarmPool<ExampleResource<>> pool(1, 1);
    auto value = pool.get();
    CHECK(pool.wait_get(0ms) == nullptr && pool.wait_get(-5ms) == nullptr);
    const auto start = std::chrono::steady_clock::now();
    CHECK(pool.wait_get(3ms) == nullptr);
    CHECK(std::chrono::steady_clock::now() - start < 2s);
    const auto generation = value->generation();
    std::thread thread([&pool, value, generation] {
        std::this_thread::sleep_for(10ms);
        CHECK(value->end_work(generation));
        CHECK(pool.recycle(value));
    });
    auto acquired = pool.wait_get(std::chrono::milliseconds::max());
    CHECK(acquired != nullptr);
    release(pool, acquired);
    thread.join();
}

void generation_limit() {
    WarmPool<ExampleResource<>> pool(1, 1);
    WarmPoolTestAccess::exhaust_next(pool);
    auto value = pool.get();
    CHECK(value && value->generation() == std::numeric_limits<std::uint64_t>::max());
    release(pool, value);
    CHECK(pool.get() == nullptr);
    CHECK(pool.get() == nullptr && pool.pending_count() == 0);
    CHECK(pool.in_use_count() == 0 && pool.free_resource_count() == 1);
}

void concurrent() {
    constexpr std::size_t producers = 4, consumers = 4, per_producer = 4000;
    Control control;
    {
        Pool pool(2, 16, {&control}, 4);
        pool.set_release_callback([](void* value) noexcept {
            ++static_cast<Control*>(value)->recycles;
        }, &control);
        using Loan = Pool::Loan;
        std::deque<Loan> queue;
        std::unordered_set<Payload*> active;
        std::mutex mutex;
        std::condition_variable available;
        bool closed = false;
        std::array<std::atomic<unsigned>, producers * per_producer> seen{};
        std::array<std::thread, consumers> readers;
        for (auto& reader : readers) reader = std::thread([&] {
            for (;;) {
                Loan loan;
                {
                    std::unique_lock lock(mutex);
                    available.wait(lock, [&] { return closed || !queue.empty(); });
                    if (queue.empty()) return;
                    loan = queue.front(); queue.pop_front();
                }
                const auto id = loan->data[0];
                CHECK(id < seen.size());
                CHECK(loan->generation() == loan.generation());
                for (std::size_t i = 1; i < 8; ++i) CHECK(loan->data[i] == (~id ^ i));
                CHECK(seen[id].fetch_add(1) == 0);
                {
                    std::lock_guard lock(mutex);
                    CHECK(active.erase(loan.get()) == 1);
                }
                CHECK(loan->end_work(loan.generation()));
                CHECK(pool.recycle(loan));
                CHECK(!pool.recycle(loan));
            }
        });
        std::array<std::thread, producers> writers;
        for (std::size_t n = 0; n < producers; ++n) writers[n] = std::thread([&, n] {
            for (std::size_t j = 0; j < per_producer; ++j) {
                auto value = pool.wait_get(10s);
                CHECK(value);
                {
                    std::lock_guard lock(mutex);
                    CHECK(active.insert(value.get()).second);
                }
                const std::uint64_t id = n * per_producer + j;
                value->data[0] = id;
                for (std::size_t i = 1; i < 8; ++i) value->data[i] = ~id ^ i;
                {
                    std::lock_guard lock(mutex);
                    queue.push_back(value);
                }
                available.notify_one();
                CHECK(pool.resource_count() <= 16);
            }
        });
        for (auto& writer : writers) writer.join();
        {
            std::lock_guard lock(mutex); closed = true;
        }
        available.notify_all();
        for (auto& reader : readers) reader.join();
        CHECK(active.empty() && pool.all_released() && pool.pending_count() == 0);
        CHECK(pool.in_use_count() == 0 && control.recycles == producers * per_producer);
        CHECK(pool.resource_count() >= 2 && pool.resource_count() <= 4);
        for (const auto& count : seen) CHECK(count == 1);
    }
    CHECK(control.creates == control.destroys);
    CHECK(control.creates == control.disposes);
}

void dispose_lifecycle() {
    Control control;
    {
        Pool pool(0, 3, {&control}, 1);
        pool.set_release_callback([](void* value) noexcept {
            ++static_cast<Control*>(value)->recycles;
        }, &control);
        auto first = pool.get();
        finish(first);
        CHECK(pool.recycle(first));
        CHECK(pool.free_resource_count() == 1 && control.disposes == 0);
        auto reused = pool.get();
        CHECK(reused.get() == first.get());
        finish(reused);
        CHECK(pool.recycle(reused) && control.disposes == 0);
        std::array<Pool::Loan, 3> values;
        for (auto& value : values) value = pool.get();
        for (const auto& value : values) release(pool, value);
        CHECK(control.disposes == 1 && control.destroys == 1);
        CHECK(pool.free_resource_count() == 1 && pool.resource_count() == 1);
    }
    CHECK(control.creates == 3 && control.recycles == 5);
    CHECK(control.disposes == 3 && control.destroys == 3);
}

struct RawMemory : ExampleResource<> {
    Control* control;
    std::byte* bytes = nullptr;
    explicit RawMemory(Control* value) : control(value) {
        allocate_bytes();
        ++control->creates;
    }
    ~RawMemory() noexcept {
        free_bytes(); // Factory::dispose invokes destruction and releases every resource.
        ++control->destroys;
    }
    bool reset(std::uint64_t generation) noexcept {
        // Simulate rebuilding internal data before lending and failing after partial preparation.
        if (control->reset_mode != 0) {
            free_bytes();
            try { allocate_bytes(); } catch (...) { return false; }
            return false;
        }
        return ExampleResource<>::reset(generation);
    }
private:
    void allocate_bytes() {
        CHECK(bytes == nullptr);
        bytes = new std::byte[64];
        ++control->allocations;
    }
    void free_bytes() noexcept {
        if (bytes) {
            delete[] bytes;
            bytes = nullptr;
            ++control->frees;
        }
    }
};

void dispose_raw_memory() {
    using RawPool = WarmPool<RawMemory, true, Factory<RawMemory>>;
    Control control;
    {
        RawPool pool(0, 1, {&control}, 0);
        auto original = pool.get();
        CHECK(original && original->bytes);
        release(pool, original);
        CHECK(control.frees == 0 && pool.will_dispose_count() == 1);
        auto next = pool.get();
        CHECK(next && control.creates == 2 && !pool.is_valid(original));
        release(pool, next);
        CHECK(control.frees == 1 && control.destroys == 1);
        for (int mode : {1, 2}) {
            control.create_mode = mode;
            CHECK(!pool.get() && pool.pending_count() == 0);
        }
        control.create_mode = 0;
        control.reset_mode = 1;
        CHECK(!pool.get()); // Factory still destroys an object whose reset partially fails.
        CHECK(control.creates == 3 && control.destroys == 2);
        CHECK(control.allocations == control.frees + 1);
        control.reset_mode = 0;
        release(pool, pool.get());
    }
    CHECK(control.creates == 4 && control.destroys == 4 && control.disposes == 4);
    CHECK(control.allocations == 5 && control.frees == 5);
}

void dispose_ctor_failure() {
    struct FailAfterFactory {
        Control* control;
        int attempts = 0;
        Payload* create() {
            if (++attempts == 3) throw std::bad_alloc{};
            return new Payload(control);
        }
        void dispose(Payload* value) const noexcept { ++control->disposes; delete value; }
    };
    Control control;
    bool rejected = false;
    try { WarmPool<Payload, true, FailAfterFactory> pool(3, 3, {&control}); }
    catch (const std::bad_alloc&) { rejected = true; }
    CHECK(rejected && control.creates == 2);
    CHECK(control.disposes == 2 && control.destroys == 2);
    {
        Pool pool(0, 1, {&control});
        control.reset_mode = 1;
        CHECK(pool.get() == nullptr);
        CHECK(control.disposes == 2 && control.destroys == 2 && pool.pending_count() == 0);
    }
    CHECK(control.disposes == 3 && control.destroys == 3);
}

void duplicate_recycle() {
    // Cover retained-object and destroy-on-return paths; both returner threads use tickets without touching resources.
    for (const std::size_t cap : {0u, 1u}) {
        Control control;
        Pool pool(0, 1, {&control}, cap);
        pool.set_release_callback([](void* p) noexcept { ++static_cast<Control*>(p)->recycles; }, &control);
        std::barrier barrier(3);
        Pool::Loan loan;
        std::atomic<unsigned> successes{0};
        auto run = [&] {
            for (unsigned i = 0; i < 2000; ++i) {
                barrier.arrive_and_wait();
                if (pool.recycle(loan)) ++successes;
                barrier.arrive_and_wait();
            }
        };
        std::thread a(run), b(run);
        for (unsigned i = 0; i < 2000; ++i) {
            auto old = loan;
            loan = pool.get();
            CHECK(loan);
            finish(loan);
            CHECK(!pool.recycle(old)); // A newer round has ended; an old generation must still not return it.
            barrier.arrive_and_wait();
            barrier.arrive_and_wait();
            CHECK(successes == i + 1 && control.recycles == static_cast<int>(i + 1));
            CHECK(!pool.is_valid(loan) && !pool.recycle(loan) && pool.all_released());
            CHECK(pool.free_resource_count() == cap && pool.pending_count() == 0);
        }
        a.join(); b.join();
        CHECK(control.disposes == (cap ? 0 : 1999));
    }
}

void stale_during_reset() {
    Control control;
    Pool pool(1, 1, {&control});
    auto old = pool.get();
    release(pool, old);
    std::latch entered(1), resume(1);
    control.on_reset = [&] { entered.count_down(); resume.wait(); };
    Pool::Loan current;
    std::thread borrower([&] { current = pool.get(); });
    entered.wait();
    CHECK(!pool.is_valid(old) && !pool.recycle(old));
    CHECK(pool.pending_count() == 1 && pool.in_use_count() == 0);
    resume.count_down(); borrower.join();
    control.on_reset = {};
    release(pool, current);
}

void factory_loan() {
    struct HandleFactory {
        Control* control;
        Loan<void>* saved;
        Payload* create(Loan<void> handle) const {
            *saved = handle;
            return new Payload(control);
        }
        void dispose(Payload* value) const noexcept { ++control->disposes; delete value; }
    };
    Control control;
    Loan<void> handle;
    using HandlePool = WarmPool<Payload, true, HandleFactory>;
    HandlePool pool(0, 1, {&control, &handle}, 0);
    HandlePool other(0, 1, {&control, &handle}, 0);
    CHECK(!pool.is_valid(HandlePool::Loan{}));
    auto a = pool.get();
    CHECK(pool.is_valid(a));
    CHECK(!other.is_valid(*a));
    CHECK(pool.is_valid(*a) && pool.is_valid(a.get()));
    release(pool, a); // Handle validation must not access freed memory after resource destruction.
    CHECK(control.disposes == 0 && !pool.is_valid(a));
    auto b = pool.get();
    CHECK(!pool.is_valid(a));
    CHECK(pool.is_valid(*b));
    release(pool, b);
}

template<bool concurrent> void default_finish_one() {
    using R = DefaultResource<concurrent>;
    using P = WarmPool<R, concurrent>;
    for (const std::size_t cap : {0u, 1u}) {
        P pool(0, 1, {}, cap), other(0, 1);
        typename P::Loan old;
        R unbound;
        CHECK(!unbound.finish() && !pool.is_valid(unbound));
        CHECK(!pool.is_valid(static_cast<R*>(nullptr)));
        CHECK(!pool.recycle(static_cast<R*>(nullptr)));
        for (int i = 0; i < 20; ++i) {
            auto loan = pool.get();
            CHECK(loan && pool.is_valid(*loan) && pool.is_valid(loan.get()));
            CHECK(!other.is_valid(*loan) && !other.recycle(*loan));
            CHECK(!pool.is_valid(old) && !pool.recycle(old));
            CHECK(loan->finish()); // cap = 0 registers deferred destruction; never access the object after returning it.
            CHECK(pool.all_released() && pool.free_resource_count() == cap);
            CHECK(!pool.is_valid(loan) && !pool.recycle(loan));
            old = loan;
        }
    }
}

void default_finish() {
    default_finish_one<true>();
    default_finish_one<false>();
    struct Counted : DefaultResource<> {
        int* cleaned;
        explicit Counted(int* value) : cleaned(value) {}
        ~Counted() noexcept { ++*cleaned; }
    };
    struct CountingFactory {
        int* cleaned;
        Counted* create() const { return new Counted(cleaned); }
        void dispose(Counted* value) const noexcept { delete value; }
    };
    int cleaned = 0;
    WarmPool<Counted, true, CountingFactory> pool(0, 1, {&cleaned}, 0);
    auto loan = pool.get();
    CHECK(loan && loan->finish() && cleaned == 0 && pool.all_released());
    CHECK(!pool.recycle(loan) && cleaned == 0);
}


void node_lifetime() {
    for (std::size_t cache : {0u, 1u}) {
        Control control;
        {
            Pool pool(0, 1, {&control}, cache), other(0, 1, {&control});
            auto loan = pool.get();
            auto copy = loan;
            {
                std::lock_guard guard(copy);
                CHECK(loan.is_valid() && !other.is_valid(loan));
                loan->data[0] = 73;
            }
            release(pool, loan);
            {
                std::lock_guard guard(copy);
                CHECK(!copy.is_valid() && !copy.recycle());
            }
            CHECK(pool.all_released() && control.destroys == 0);
            auto next = pool.get();
            CHECK(next && next.generation() != loan.generation());
            { std::lock_guard guard(copy); CHECK(!copy.is_valid()); }
            release(pool, next);
            CHECK(control.destroys == (cache ? 0 : 1));
        }
        CHECK(control.creates == control.destroys);
    }
    WarmPool<DefaultResource<false>, false> serial(0, 1, {}, 0);
    auto loan = serial.get();
    { std::lock_guard guard(loan); CHECK(loan.is_valid()); }
    CHECK(loan.recycle() && serial.all_released());
}

void node_concurrent() {
    // get must not reset an object while entered operations from its old generation remain active.
    for (std::size_t cache : {0u, 1u}) {
        Control control;
        Pool pool(0, 2, {&control}, cache);
        auto loan = pool.get();
        auto unrelated = pool.get();
        std::latch entered(1), leave(1);
        std::thread worker([&] {
            std::lock_guard guard(loan);
            CHECK(loan.is_valid());
            entered.count_down(); leave.wait();
        });
        entered.wait();
        release(pool, loan);
        Pool::Loan next;
        std::atomic<bool> returned{false};
        std::thread borrower([&] { next = pool.get(); returned = true; });
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!pool.pending_count() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        CHECK(pool.pending_count() == 1 && !returned);
        CHECK(pool.is_valid(unrelated)); // Do not hold the pool lock while waiting for a Node lock.
        leave.count_down(); worker.join(); borrower.join();
        CHECK(next && next.generation() != loan.generation());
        { std::lock_guard guard(loan); CHECK(!loan.is_valid()); }
        release(pool, next); release(pool, unrelated);
        CHECK(pool.all_released());
    }
}

void deferred_two_candidates() {
    Control control;
    {
        Pool pool(0, 4, {&control}, 0);
        auto a = pool.get(), b = pool.get(), c = pool.get(), d = pool.get();
        std::latch entered(1), leave(1);
        std::thread busy([&] {
            std::lock_guard guard(a);
            entered.count_down(); leave.wait();
        });
        entered.wait();
        release(pool, a); // [a]
        release(pool, b); // a is busy; try it once only: [a, b].
        CHECK(control.destroys == 0 && pool.will_dispose_count() == 2);
        release(pool, c); // a is busy, but distinct second record b can be destroyed: [a, c].
        CHECK(control.destroys == 1 && pool.will_dispose_count() == 2);
        leave.count_down(); busy.join();
        release(pool, d); // Destroy old records a and c during this round: [d].
        CHECK(control.destroys == 3 && pool.will_dispose_count() == 1);
        CHECK(pool.resource_count() == 0 && pool.all_released());
    }
    CHECK(control.destroys == 4 && control.disposes == 4);
}

void retired_record_reuse() {
    Control control;
    {
        Pool pool(0, 1, {&control}, 0);
        auto old = pool.get();
        release(pool, old);
        auto current = pool.get(); // max_count = 1: the old object remains queued while its slot is reusable.
        CHECK(current && current.get() != old.get() && control.destroys == 0);
        current->data[0] = 42;
        { std::lock_guard guard(old); CHECK(!old.is_valid()); }
        release(pool, current); // Destroy old's pointer, not Node's current pointer twice.
        CHECK(control.destroys == 1 && pool.will_dispose_count() == 1);
        auto last = pool.get();
        CHECK(last && last->data[0] == 0);
        release(pool, last);
    }
    CHECK(control.creates == 3 && control.destroys == 3);
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string_view name(argv[1]);
    if (name == "protocol") protocol();
    else if (name == "resource") resource();
    else if (name == "lifecycle") lifecycle();
    else if (name == "independent") independent();
    else if (name == "failures") failures();
    else if (name == "factory_dispose") factory_dispose();
    else if (name == "trim_destroy") trim_destroy();
    else if (name == "trim_recreate") trim_recreate();
    else if (name == "slow_hooks") slow_hooks();
    else if (name == "trim_overlap") trim_overlap();
    else if (name == "wait") wait_case();
    else if (name == "generation_limit") generation_limit();
    else if (name == "concurrent") concurrent();
    else if (name == "dispose_lifecycle") dispose_lifecycle();
    else if (name == "dispose_raw_memory") dispose_raw_memory();
    else if (name == "dispose_ctor_failure") dispose_ctor_failure();
    else if (name == "duplicate_recycle") duplicate_recycle();
    else if (name == "stale_during_reset") stale_during_reset();
    else if (name == "factory_loan") factory_loan();
    else if (name == "default_finish") default_finish();
    else if (name == "node_lifetime") node_lifetime();
    else if (name == "node_concurrent") node_concurrent();
    else if (name == "deferred_two_candidates") deferred_two_candidates();
    else if (name == "retired_record_reuse") retired_record_reuse();
    else if (name == "death_live") {
        std::set_terminate([] { std::_Exit(91); });
        WarmPool<ExampleResource<>> pool(0, 1);
        CHECK(pool.get());
    }
    else return 2;
    std::cout << "PASS " << name << '\n';
}
