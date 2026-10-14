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
template<class T>
concept PoolResource = std::is_nothrow_destructible_v<T> &&
    requires(T& resource, std::uint64_t generation) {
        { resource.reset(generation) } noexcept -> std::same_as<bool>;
    };

// Declare the constrained template before Loan friendship (Apple Clang compatibility).
template<PoolResource TResource, bool TIsConcurrent, class Factory> class WarmPool;

// Loan operation entry points; owns neither pool, node nor resource and needs no separate Handle type.
struct LoanOperations {
    bool (*is_valid)(const LoanOperations&, void*, void*, std::uint64_t) noexcept;
    bool (*recycle)(const LoanOperations&, void*, void*, std::uint64_t) noexcept;
    void* context = nullptr;
};

template<class T> class Loan {
    template<class> friend class Loan;
    template<PoolResource TResource, bool TIsConcurrent, class Factory> friend class WarmPool;
    template<class Pool, class Queue> friend class PooledResourceQueue;
    Loan(void* owner, void* node, T* resource, std::uint64_t generation,
         SpinLock* lock, const LoanOperations* operations) noexcept
        : owner_(owner), node_(node), resource_(resource), generation_(generation),
          lock_(lock), operations_(operations) {}
public:
    Loan() noexcept = default;
    template<class U> requires std::is_convertible_v<U*, T*>
    Loan(const Loan<U>& other) noexcept
        : owner_(other.owner_), node_(other.node_), resource_(other.resource_),
          generation_(other.generation_), lock_(other.lock_), operations_(other.operations_) {}

    // Return the loan-time snapshot without extending lifetime; the pool and operation-table queue must outlive ticket operations.
    [[nodiscard]] explicit operator bool() const noexcept { return resource_ != nullptr; }
    [[nodiscard]] bool operator==(std::nullptr_t) const noexcept { return resource_ == nullptr; }
    [[nodiscard]] T* get() const noexcept { return resource_; }
    [[nodiscard]] T* operator->() const noexcept { return resource_; }
    template<class U = T> requires (!std::is_void_v<U>)
    [[nodiscard]] U& operator*() const noexcept { return *resource_; }
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
    T* resource_ = nullptr;
    std::uint64_t generation_ = 0;
    SpinLock* lock_ = nullptr; // nullptr for serialized pools; no locking.
    const LoanOperations* operations_ = nullptr;
};

template<class T>
concept BoundResource = requires(const T& resource) {
    { resource.loan() } noexcept -> std::convertible_to<Loan<void>>;
};

// Optional compatibility helper; WarmPool resources do not need to inherit it.
template<bool is_concurrent = true>
class DefaultResource {
public:
    static constexpr bool concurrent = is_concurrent;
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
    std::conditional_t<is_concurrent, std::atomic<std::uint64_t>, PlainValue<std::uint64_t>> generation_{0};
};

// A successful create transfers ownership to the pool; dispose fully destroys it and the pool never deletes it again.
template<class T> struct DefaultResourceFactory {
    T* create() const { return new T; }
    void dispose(T* resource) const noexcept { delete resource; }
};
} // namespace TwoBufQueue
