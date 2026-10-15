#pragma once

#include "TwoBufQueue/src/utils/plain_value.hpp"
#include "TwoBufQueue/src/utils/spin_lock.hpp"
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace TwoBufQueue {

// Nonintrusive resource protocol: resources implement reset without inheritance or embedded pool metadata.
template<class TValue>
concept PoolResource = std::is_nothrow_destructible_v<TValue> &&
    requires(TValue& resource, std::uint64_t generation) {
        { resource.reset(generation) } noexcept -> std::same_as<bool>;
    };

// Declare the constrained template before Loan friendship (Apple Clang compatibility).
template<PoolResource TResource, bool TIsConcurrent, class TFactory> class WarmPool;

// Loan operation entry points; owns neither pool, node nor resource and needs no separate Handle type.
struct LoanOperations {
    bool (*is_valid)(const LoanOperations&, void*, void*, std::uint64_t) noexcept;
    bool (*recycle)(const LoanOperations&, void*, void*, std::uint64_t) noexcept;
    void* context = nullptr;
};

template<class TValue> class Loan {
    template<class> friend class Loan;
    template<PoolResource TResource, bool TIsConcurrent, class TFactory> friend class WarmPool;
    template<class TPool, class TQueue> friend class PooledResourceQueue;
    Loan(void* owner, void* node, TValue* resource, std::uint64_t generation,
         SpinLock* lock, const LoanOperations* operations) noexcept
        : owner_(owner), node_(node), resource_(resource), generation_(generation),
          lock_(lock), operations_(operations) {}
public:
    Loan() noexcept = default;
    template<class TOtherValue> requires std::is_convertible_v<TOtherValue*, TValue*>
    Loan(const Loan<TOtherValue>& other) noexcept
        : owner_(other.owner_), node_(other.node_), resource_(other.resource_),
          generation_(other.generation_), lock_(other.lock_), operations_(other.operations_) {}

    // Return the loan-time snapshot without extending lifetime; the pool and operation-table queue must outlive ticket operations.
    [[nodiscard]] explicit operator bool() const noexcept { return resource_ != nullptr; }
    [[nodiscard]] bool operator==(std::nullptr_t) const noexcept { return resource_ == nullptr; }
    [[nodiscard]] TValue* get() const noexcept { return resource_; }
    [[nodiscard]] TValue* operator->() const noexcept { return resource_; }
    template<class TOtherValue = TValue> requires (!std::is_void_v<TOtherValue>)
    [[nodiscard]] TOtherValue& operator*() const noexcept { return *resource_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] bool is_valid() const noexcept {
        return operations_ && operations_->is_valid(*operations_, owner_, node_, generation_);
    }
    [[nodiscard]] bool recycle() const noexcept {
        return operations_ && operations_->recycle(*operations_, owner_, node_, generation_);
    }
    // Ordinary pool loans return to the pool; HandoffQueue receive loans also complete their queue's accounting.
    [[nodiscard]] bool finish() const noexcept { return recycle(); }
    // The stable Node owns the lock. Loan copies use the same lock; lock before validating the generation.
    // Protect only accesses participating through this synchronization entry point; never access a returned resource.
    void lock() const noexcept { if (lock_) lock_->lock(); }
    [[nodiscard]] bool try_lock() const noexcept { return !lock_ || lock_->try_lock(); }
    void unlock() const noexcept { if (lock_) lock_->unlock(); }
private:
    void* owner_ = nullptr;
    void* node_ = nullptr;
    TValue* resource_ = nullptr;
    std::uint64_t generation_ = 0;
    SpinLock* lock_ = nullptr; // nullptr for serialized pools; no locking.
    const LoanOperations* operations_ = nullptr;
};

template<class TValue>
concept BoundResource = requires(const TValue& resource) {
    { resource.loan() } noexcept -> std::convertible_to<Loan<void>>;
};

// Optional compatibility helper; WarmPool resources do not need to inherit it.
template<bool TIsConcurrent = true>
class DefaultResource {
public:
    static constexpr bool concurrent = TIsConcurrent;
    DefaultResource() noexcept = default;
    DefaultResource(const DefaultResource&) = delete;
    DefaultResource& operator=(const DefaultResource&) = delete;
    // Update the ticket on every loan; Slice/callers must retain copies rather than references to this replaceable member.
    void bind(Loan<void> loan) noexcept { loan_ = loan; }
    bool reset(std::uint64_t generation) noexcept {
        if (!generation) return false;
        generation_.store(generation, std::memory_order_release);
        return true;
    }
    [[nodiscard]] Loan<void> loan() const noexcept { return loan_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }
    bool finish() noexcept {
        const auto saved = loan_; // Do not access this after returning it; the caller must finish all other uses first.
        return saved.recycle();
    }
protected:
    Loan<void> loan_;
    std::conditional_t<TIsConcurrent, std::atomic<std::uint64_t>, PlainValue<std::uint64_t>> generation_{0};
};

// A successful create transfers ownership to the pool; dispose fully destroys it and the pool never deletes it again.
template<class TValue> struct DefaultResourceFactory {
    TValue* create() const { return new TValue; }
    void dispose(TValue* resource) const noexcept { delete resource; }
};
} // namespace TwoBufQueue
