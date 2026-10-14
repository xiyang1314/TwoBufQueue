#pragma once

#include "resource.hpp"
#if defined(BRQ_ENABLE_POOL_DEPTH_STATS)
#include "depth_stats.hpp"
#endif
#include "intrusive_stack.hpp"
#include "intrusive_queue.hpp"
#include "TwoBufQueue/src/utils/wait.hpp"
#include "TwoBufQueue/src/utils/no_lock.hpp"
#include <chrono>
#include <cstdio>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace TwoBufQueue {

enum class PoolGetStatus { Success, Limit, ResourceError };

// The pool lock protects containers/statistics; Node locks protect resource operations using Loan synchronization.
// Lock a Loan before checking validity; callers using ordinary raw pointers remain responsible for lifetime.
// Creation, reset and Factory::dispose run outside the pool lock; factories must support concurrent calls.
template<PoolResource TResource, bool TIsConcurrent = true,
         class Factory = DefaultResourceFactory<TResource>>
class WarmPool {
    using Lock = std::conditional_t<TIsConcurrent, SpinLock, NoLock>;
    struct Node;
    // Allocate alongside creation so destruction registration needs no allocation. Old records are independent of a reused Node::record.
    struct Record {
        Node* node;
        TResource* resource = nullptr;
        Record* next = nullptr;
    };
    struct Node {
        Lock lock_; // Stable address until pool destruction, allowing old Loans to lock safely.
        Record* record = nullptr;
        Node* next = nullptr;
        std::uint64_t generation = 0;
        bool in_use = false;
    };
    friend struct WarmPoolTestAccess;
    static_assert(requires(Factory& factory, TResource* resource) {
        { factory.dispose(resource) } noexcept -> std::same_as<void>;
    }, "Factory::dispose(Resource*) must be noexcept and destroy the complete resource");

public:
    static constexpr bool concurrent = TIsConcurrent;
    using ResourceType = TResource;
    using Loan = TwoBufQueue::Loan<TResource>;
    using ReleaseCallback = void (*)(void*) noexcept;

    WarmPool(std::size_t min_count, std::size_t max_count, Factory factory = {},
        std::size_t max_free_count = std::numeric_limits<std::size_t>::max())
        : min_count_(min_count), max_count_(max_count), max_free_count_(max_free_count),
          factory_(std::move(factory)) {
        if (!max_count || min_count > max_count || max_count == std::numeric_limits<std::size_t>::max())
            throw std::invalid_argument("WarmPool requires 0 <= min_count <= max_count < SIZE_MAX and max_count > 0");
        nodes_.reserve(max_count);
        try {
            if (!preallocate(min_count)) throw std::bad_alloc{};
        } catch (...) {
            destroy_all(); // Raw-pointer members do not clean themselves up; failed construction does not invoke this object's destructor.
            throw;
        }
    }
    WarmPool(const WarmPool&) = delete;
    WarmPool& operator=(const WarmPool&) = delete;
    ~WarmPool() noexcept {
        // Destroy the pool only after all calls/Loan-lock operations stop and every loan is returned.
        if (in_use_count_ || pending_count_) {
            std::fputs("destroying WarmPool while resources are still in use\n", stderr);
            std::terminate();
        }
        destroy_all(); // Destroy the final pending objects now rather than waiting for another registration.
    }

    [[nodiscard]] Loan get() noexcept {
        PoolGetStatus ignored;
        return get(ignored);
    }
    // Record the reason at the failure site instead of inferring concurrent state from counters afterward.
    [[nodiscard]] Loan get(PoolGetStatus& status) noexcept {
        status = PoolGetStatus::ResourceError;
        auto* node = reserve_node(true, &status);
        if (!node) return {};
        // Wait for prior-generation node operations to leave, without holding the pool lock or allocating under the node lock.
        { std::lock_guard<Lock> guard(node->lock_); }
        if (!prepare(node)) return {};
        auto loan = make_loan(node, node->record->resource);
        if constexpr (requires(TResource& r) { { r.bind(loan) } noexcept -> std::same_as<void>; })
            node->record->resource->bind(loan);
        if (!node->record->resource->reset(loan.generation())) {
            discard(node);
            return {};
        }
        std::lock_guard<Lock> guard(lock_);
        node->in_use = true;
        ++in_use_count_;
        --pending_count_;
#if defined(BRQ_ENABLE_POOL_DEPTH_STATS)
        if (depth_stats_enabled_) depth_stats_.record(in_use_count_, resource_count_, free_count_);
#endif
        status = PoolGetStatus::Success;
        return loan;
    }
    template<class Rep, class Period>
    [[nodiscard]] Loan wait_get(std::chrono::duration<Rep, Period> timeout) noexcept {
        using Clock = std::chrono::steady_clock;
        const auto deadline = deadline_after<Clock>(Clock::now(), wait_duration<Clock>(timeout));
        const auto interval = wait_duration<Clock>(std::chrono::milliseconds{1});
        for (;;) {
            if (auto loan = get()) return loan;
            const auto now = Clock::now();
            if (now >= deadline) return {};
            const auto retry = deadline_after<Clock>(now, interval);
            std::this_thread::sleep_until(retry < deadline ? retry : deadline);
        }
    }

    // O(1) snapshot check of stable nodes only; the resource pointer may already be stale.
    template<class U> [[nodiscard]] bool is_valid(const TwoBufQueue::Loan<U>& loan) const noexcept {
        if (!owns(loan)) return false;
        return is_valid_node(static_cast<const Node*>(loan.node_), loan.generation_);
    }
    [[nodiscard]] bool is_valid(const TResource& resource) const noexcept requires BoundResource<TResource> {
        return is_valid(resource.loan());
    }
    [[nodiscard]] bool is_valid(const TResource* resource) const noexcept requires BoundResource<TResource> {
        return resource && is_valid(*resource);
    }
    // Claim return at most once per generation. Stop using the resource afterward; ordinary accesses require caller serialization.
    // A thread holding an old Loan lock may unlock it; get/destruction waits or skips the node so generations never overlap.
    template<class U> [[nodiscard]] bool recycle(const TwoBufQueue::Loan<U>& loan) noexcept {
        return owns(loan) && recycle_node(static_cast<Node*>(loan.node_), loan.generation_);
    }
    [[nodiscard]] bool recycle(const TResource& resource) noexcept requires BoundResource<TResource> {
        const auto loan = resource.loan();
        return recycle(loan);
    }
    [[nodiscard]] bool recycle(const TResource* resource) noexcept requires BoundResource<TResource> {
        return resource && recycle(*resource);
    }
    // Call during initialization/quiescence; count is the pool's total resource count, excluding old objects pending destruction.
    [[nodiscard]] bool preallocate(std::size_t count) noexcept {
        if (count > max_count_) return false;
        while (resource_count() < count) {
            auto* node = reserve_node(false);
            if (!node || !prepare(node)) return false;
            std::lock_guard<Lock> guard(lock_);
            put_free_without_lock(node);
        }
        return true;
    }
    // Notify once after logical return; physical destruction may be deferred. Query only; do not reenter mutations.
    void set_release_callback(ReleaseCallback callback, void* context) noexcept {
        std::lock_guard<Lock> guard(lock_);
        release_callback_ = callback;
        release_context_ = context;
    }
    [[nodiscard]] const Factory& factory() const noexcept { return factory_; }
#if defined(BRQ_ENABLE_POOL_DEPTH_STATS)
    // Begin only after warmup and input preparation. The initial snapshot
    // contributes to maxima, not to acquisition-weighted averages.
    void begin_depth_stats() noexcept {
        std::lock_guard<Lock> guard(lock_);
        depth_stats_.begin(in_use_count_, resource_count_);
        depth_stats_enabled_ = true;
    }
    [[nodiscard]] PoolDepthStats end_depth_stats() noexcept {
        std::lock_guard<Lock> guard(lock_);
        depth_stats_enabled_ = false;
        return depth_stats_;
    }
#endif
    [[nodiscard]] std::size_t min_count() const noexcept { return min_count_; }
    [[nodiscard]] std::size_t max_count() const noexcept { return max_count_; }
    [[nodiscard]] std::size_t resource_count() const noexcept {
        std::lock_guard<Lock> guard(lock_); return resource_count_;
    }
    [[nodiscard]] std::size_t free_resource_count() const noexcept {
        std::lock_guard<Lock> guard(lock_); return free_count_;
    }
    [[nodiscard]] std::size_t in_use_count() const noexcept {
        std::lock_guard<Lock> guard(lock_); return in_use_count_;
    }
    [[nodiscard]] std::size_t pending_count() const noexcept {
        std::lock_guard<Lock> guard(lock_); return pending_count_;
    }
    [[nodiscard]] std::size_t will_dispose_count() const noexcept {
        std::lock_guard<Lock> guard(lock_); return will_dispose_qu_.size();
    }
    [[nodiscard]] bool all_released() const noexcept {
        std::lock_guard<Lock> guard(lock_); return !in_use_count_ && !pending_count_;
    }
    template<class U> [[nodiscard]] bool owns(const TwoBufQueue::Loan<U>& loan) const noexcept {
        return loan.owner_ == this && loan.node_ != nullptr;
    }
    // The raw resource must still be alive; this overload cannot detect dangling pointers.
    [[nodiscard]] bool owns(const TResource& resource) const noexcept requires BoundResource<TResource> {
        return owns(resource.loan());
    }
    [[nodiscard]] bool owns(const TResource* resource) const noexcept requires BoundResource<TResource> {
        return resource && owns(*resource);
    }

private:
    Loan make_loan(Node* node, TResource* resource) noexcept {
        SpinLock* gate = nullptr;
        if constexpr (TIsConcurrent) gate = &node->lock_;
        return Loan(this, node, resource, node->generation, gate, &operations_);
    }
    bool is_valid_node(const Node* node, std::uint64_t generation) const noexcept {
        std::lock_guard<Lock> guard(lock_);
        return node->in_use && node->generation == generation;
    }
    bool recycle_node(Node* node, std::uint64_t generation) noexcept {
        Record* retiring = nullptr;
        ReleaseCallback callback;
        void* context;
        {
            std::lock_guard<Lock> guard(lock_);
            if (!node->in_use || node->generation != generation) return false;
            node->in_use = false;
            --in_use_count_;
            ++pending_count_;
            if (free_count_ >= max_free_count_ && resource_count_ > min_count_) {
                retiring = std::exchange(node->record, nullptr);
                --resource_count_; // Reserve shrinking immediately so concurrent returns cannot go below min_count.
            } else {
                put_free_without_lock(node);
            }
            callback = release_callback_;
            context = release_context_;
        }
        if (retiring) {
            register_dispose(retiring);
            std::lock_guard<Lock> guard(lock_);
            dormant_.push(node); // The queue retains the old Record while the node may create a new resource.
            --pending_count_;
        }
        if (callback) callback(context);
        return true;
    }
    void put_free_without_lock(Node* node) noexcept {
        free_.push(node); ++free_count_; --pending_count_;
    }
    Node* reserve_node(bool borrow, PoolGetStatus* status = nullptr) noexcept {
        std::uint64_t generation = 0;
        {
            std::lock_guard<Lock> guard(lock_);
            if (borrow && next_generation_ == std::numeric_limits<std::uint64_t>::max()) return nullptr;
            auto* node = borrow ? free_.pop() : nullptr;
            if (node) --free_count_;
            else node = dormant_.pop();
            if (node) {
                ++pending_count_;
                if (borrow) node->generation = ++next_generation_;
                return node;
            }
            if (nodes_.size() + creating_count_ >= max_count_) {
                if (status) *status = PoolGetStatus::Limit;
                return nullptr;
            }
            ++creating_count_; ++pending_count_;
            if (borrow) generation = ++next_generation_;
        }
        Node* node = nullptr;
        try { node = new Node; } catch (...) {}
        std::lock_guard<Lock> guard(lock_);
        --creating_count_;
        if (!node) { --pending_count_; return nullptr; }
        node->generation = generation;
        nodes_.push_back(node); // reserve(max_count) ran during construction; no allocation here.
        return node;
    }
    bool prepare(Node* node) noexcept {
        if (node->record) return true;
        Record* record = nullptr;
        try {
            record = new Record{node};
            const auto seed = make_loan(node, nullptr);
            if constexpr (requires { factory_.create(TwoBufQueue::Loan<void>(seed)); })
                record->resource = factory_.create(TwoBufQueue::Loan<void>(seed));
            else record->resource = factory_.create();
        } catch (...) {}
        if (!record || !record->resource) {
            delete record;
            std::lock_guard<Lock> guard(lock_);
            dormant_.push(node); --pending_count_;
            return false;
        }
        std::lock_guard<Lock> guard(lock_);
        node->record = record; ++resource_count_;
        return true;
    }
    void discard(Node* node) noexcept {
        Record* record;
        {
            std::lock_guard<Lock> guard(lock_);
            record = std::exchange(node->record, nullptr);
            --resource_count_;
        }
        register_dispose(record);
        std::lock_guard<Lock> guard(lock_);
        dormant_.push(node); --pending_count_;
    }
    void register_dispose(Record* record) noexcept {
        Record* candidates[2]{};
        {
            std::lock_guard<Lock> guard(lock_);
            // Remove two distinct old records at once; do not recheck a busy, requeued record during the same pass.
            candidates[0] = will_dispose_qu_.pop();
            candidates[1] = will_dispose_qu_.pop();
        }
        for (auto* old : candidates) {
            if (!old) continue;
            if (old->node->lock_.try_lock()) {
                // The old generation is invalid and detached from its node; acquiring the stable node lock proves old operations have exited.
                // Unlock before destruction. Later old Loans fail validation and cannot touch the old object.
                old->node->lock_.unlock();
                factory_.dispose(old->resource);
                delete old;
            } else {
                std::lock_guard<Lock> guard(lock_);
                will_dispose_qu_.push(old);
            }
        }
        std::lock_guard<Lock> guard(lock_);
        will_dispose_qu_.push(record);
    }
    void destroy_all() noexcept {
        while (auto* record = will_dispose_qu_.pop()) {
            factory_.dispose(record->resource); delete record;
        }
        for (auto* node : nodes_) {
            if (node->record) {
                factory_.dispose(node->record->resource);
                delete node->record;
            }
            delete node;
        }
        nodes_.clear();
    }
    static bool validate_loan(const LoanOperations&, void* owner, void* node, std::uint64_t generation) noexcept {
        return static_cast<WarmPool*>(owner)->is_valid_node(static_cast<Node*>(node), generation);
    }
    static bool recycle_loan(const LoanOperations&, void* owner, void* node, std::uint64_t generation) noexcept {
        return static_cast<WarmPool*>(owner)->recycle_node(static_cast<Node*>(node), generation);
    }
    inline static constexpr LoanOperations operations_{&validate_loan, &recycle_loan};
    const std::size_t min_count_;
    const std::size_t max_count_;
    const std::size_t max_free_count_;
    Factory factory_;
    mutable Lock lock_;
    std::vector<Node*> nodes_;
    SingleThreadStack<Node, &Node::next> free_;
    SingleThreadStack<Node, &Node::next> dormant_;
    SingleThreadQueue<Record, &Record::next> will_dispose_qu_;
    std::size_t free_count_ = 0, in_use_count_ = 0, resource_count_ = 0;
    std::size_t creating_count_ = 0, pending_count_ = 0;
    std::uint64_t next_generation_ = 0;
    ReleaseCallback release_callback_ = nullptr;
    void* release_context_ = nullptr;
#if defined(BRQ_ENABLE_POOL_DEPTH_STATS)
    PoolDepthStats depth_stats_{};
    bool depth_stats_enabled_ = false;
#endif
};
} // namespace TwoBufQueue
