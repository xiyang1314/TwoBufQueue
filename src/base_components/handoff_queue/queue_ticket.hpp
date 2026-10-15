#pragma once

#include "TwoBufQueue/src/base_components/warm_pool/resource.hpp"
#include <limits>

namespace TwoBufQueue {

template<class TResource> class ClaimQueue;

// Addresses and identifiers belong to the queue; copying a ticket extends neither queue nor resource lifetime.
class QueueReservation {
    template<class> friend class ClaimQueue;
public:
    explicit operator bool() const noexcept { return owner_ != nullptr; }
private:
    const void* owner_ = nullptr;
    std::size_t index_ = std::numeric_limits<std::size_t>::max();
    std::uint64_t id_ = 0;
};

template<class TResource, bool TIsClaim>
class QueueTicket {
    template<class> friend class ClaimQueue;
public:
    QueueTicket() noexcept = default;
    TResource* get() const noexcept { return loan_.get(); }
    TResource* operator->() const noexcept { return get(); }
    TResource& operator*() const noexcept { return *get(); }
    explicit operator bool() const noexcept { return bool(loan_); }
private:
    const void* owner_ = nullptr;
    std::size_t index_ = std::numeric_limits<std::size_t>::max();
    std::uint64_t id_ = 0;
    Loan<TResource> loan_;
};

// begin_finish invalidates old producer/claim tickets; complete_finish releases the slot after external recycling finishes.
template<class TResource>
class QueueCompletion {
    template<class> friend class ClaimQueue;
public:
    explicit operator bool() const noexcept { return owner_ != nullptr; }
    [[nodiscard]] Loan<TResource> loan() const noexcept { return loan_; }
private:
    const void* owner_ = nullptr;
    std::size_t index_ = std::numeric_limits<std::size_t>::max();
    std::uint64_t id_ = 0;
    Loan<TResource> loan_;
};

} // namespace TwoBufQueue
