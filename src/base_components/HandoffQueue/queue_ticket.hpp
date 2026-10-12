#pragma once

#include "TwoBufQueue/src/base_components/WarmPool/resource.hpp"
#include <limits>

namespace TwoBufQueue {

template<class Resource> class ClaimQueue;

// 地址及编号都属于队列；复制凭证不会延长队列或资源的寿命。
class QueueReservation {
    template<class> friend class ClaimQueue;
public:
    explicit operator bool() const noexcept { return owner_ != nullptr; }
private:
    const void* owner_ = nullptr;
    std::size_t index_ = std::numeric_limits<std::size_t>::max();
    std::uint64_t id_ = 0;
};

template<class Resource, bool IsClaim>
class QueueTicket {
    template<class> friend class ClaimQueue;
public:
    QueueTicket() noexcept = default;
    Resource* get() const noexcept { return loan_.get(); }
    Resource* operator->() const noexcept { return get(); }
    Resource& operator*() const noexcept { return *get(); }
    explicit operator bool() const noexcept { return bool(loan_); }
private:
    const void* owner_ = nullptr;
    std::size_t index_ = std::numeric_limits<std::size_t>::max();
    std::uint64_t id_ = 0;
    Loan<Resource> loan_;
};

// begin_finish 已使旧生产/领取凭证失效；外部完成回收后，才能 complete_finish 释放槽位。
template<class Resource>
class QueueCompletion {
    template<class> friend class ClaimQueue;
public:
    explicit operator bool() const noexcept { return owner_ != nullptr; }
    [[nodiscard]] Loan<Resource> loan() const noexcept { return loan_; }
private:
    const void* owner_ = nullptr;
    std::size_t index_ = std::numeric_limits<std::size_t>::max();
    std::uint64_t id_ = 0;
    Loan<Resource> loan_;
};

} // namespace TwoBufQueue
