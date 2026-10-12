#pragma once

#include "TwoBufQueue/src/utils/plain_value.hpp"
#include "TwoBufQueue/src/utils/spin_lock.hpp"
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace TwoBufQueue {

// 非侵入式资源协议：只要求资源自己实现 reset，不要求继承或保存池元数据。
template<class T>
concept PoolResource = std::is_nothrow_destructible_v<T> &&
    requires(T& resource, std::uint64_t generation) {
        { resource.reset(generation) } noexcept -> std::same_as<bool>;
    };

// Declare the constrained template before Loan friendship (Apple Clang compatibility).
template<PoolResource TResource, bool TIsConcurrent, class Factory> class WarmPool;

// Loan 的操作入口，不拥有池、节点或资源；没有额外的 Handle 类型。
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

    // 仅返回借出时的快照，不延长寿命；池以及提供操作表的接收队列必须活过凭证操作。
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
    // 普通池借用归还池；HandoffQueue receive 借用还会完成所属队列的计数。
    [[nodiscard]] bool finish() const noexcept { return recycle(); }
    // 锁属于稳定 Node。复制 Loan 仍使用同一把锁；必须先锁定，再校验代号。
    // 锁只保护通过此入口参与同步的访问。归还后不得继续访问资源。
    void lock() const noexcept { if (lock_) lock_->lock(); }
    [[nodiscard]] bool try_lock() const noexcept { return !lock_ || lock_->try_lock(); }
    void unlock() const noexcept { if (lock_) lock_->unlock(); }
private:
    void* owner_ = nullptr;
    void* node_ = nullptr;
    T* resource_ = nullptr;
    std::uint64_t generation_ = 0;
    SpinLock* lock_ = nullptr; // 串行池为 nullptr，不加锁。
    const LoanOperations* operations_ = nullptr;
};

template<class T>
concept BoundResource = requires(const T& resource) {
    { resource.loan() } noexcept -> std::convertible_to<Loan<void>>;
};

// 可选兼容工具；WarmPool 的资源无需继承它。
template<bool is_concurrent = true>
class DefaultResource {
public:
    static constexpr bool concurrent = is_concurrent;
    DefaultResource() noexcept = default;
    DefaultResource(const DefaultResource&) = delete;
    DefaultResource& operator=(const DefaultResource&) = delete;
    // 每次借出更新凭证；Slice/调用者保存自己的副本，不能引用这个可更新成员。
    void bind(Loan<void> loan) noexcept { loan_ = loan; }
    bool reset(std::uint64_t generation) noexcept {
        if (!generation) return false;
        generation_.store(generation, std::memory_order_release);
        return true;
    }
    [[nodiscard]] Loan<void> loan() const noexcept { return loan_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }
    bool finish() noexcept {
        const auto saved = loan_; // 归还后不访问 this；调用者负责结束其他使用。
        return saved.recycle();
    }
protected:
    Loan<void> loan_;
    std::conditional_t<is_concurrent, std::atomic<std::uint64_t>, PlainValue<std::uint64_t>> generation_{0};
};

// create 成功后交给池所有；dispose 负责完整销毁，池不会再 delete 同一资源。
template<class T> struct DefaultResourceFactory {
    T* create() const { return new T; }
    void dispose(T* resource) const noexcept { delete resource; }
};
} // namespace TwoBufQueue
