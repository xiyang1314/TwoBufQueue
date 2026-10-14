#pragma once

#include "handoff_types.hpp"
#include "queue_ticket.hpp"
#include "ready_index_queue.hpp"

namespace TwoBufQueue {

// Independent bounded claim queue: stores tickets only and invokes neither Pool nor user callbacks.
// QueueTail uses lock-free indices; Keep briefly coordinates dequeue with cancelled-claim publication, never business processing.
template<class Resource>
class ClaimQueue {
    static_assert(PoolResource<Resource>);
    enum class SlotState : std::uint64_t {
        Free, Reserved, Attaching, Producing, Publishing, Ready, Claimed, Finishing, Releasing
    };
    static constexpr std::uint64_t state_mask = 15;
    static constexpr std::uint64_t closed_bit = std::uint64_t{1} << 63;
public:
    using Loan = TwoBufQueue::Loan<Resource>;
    template<bool C> using Ticket = QueueTicket<Resource, C>;
    using ProducerLoan = Ticket<false>;
    using Claim = Ticket<true>;
    using Completion = QueueCompletion<Resource>;
    struct ReserveResult {
        AcquireStatus status;
        QueueReservation reservation;
        explicit operator bool() const noexcept { return status == AcquireStatus::Success; }
    };

    explicit ClaimQueue(std::size_t capacity, bool enable_stats = false,
                        CancelClaimMode cancel_claim_mode = CancelClaimMode::QueueTail)
        : free_(checked_capacity(capacity)), ready_(capacity, cancel_claim_mode),
          slots_(capacity), enable_stats_(enable_stats) {}
    ClaimQueue(const ClaimQueue&) = delete;
    ClaimQueue& operator=(const ClaimQueue&) = delete;

    [[nodiscard]] ReserveResult try_reserve() noexcept {
        // Order close against entering acquire calls; is_drained must include acquires that have not obtained a slot yet.
        if (is_closed()) return failure(AcquireStatus::Closed);
        const auto active = active_.fetch_add(1, std::memory_order_acquire);
        if (active & closed_bit) {
            leave();
            return failure(AcquireStatus::Closed);
        }
        if (active == closed_bit - 1) std::terminate();
        std::uint32_t index;
        if (!free_.try_pop(index)) {
            leave();
            return failure(AcquireStatus::QueueFull);
        }
        auto& slot = slots_[index];
        const auto id = next_id(slot.control.load(std::memory_order_relaxed));
        slot.control.store(control(id, SlotState::Reserved), std::memory_order_release);
        QueueReservation reservation;
        reservation.owner_ = this;
        reservation.index_ = index;
        reservation.id_ = id;
        return {AcquireStatus::Success, reservation};
    }
    [[nodiscard]] ProducerLoan attach(QueueReservation& reservation, Loan loan) noexcept {
        if (!loan || !transition(reservation, SlotState::Reserved, SlotState::Attaching)) return {};
        auto& slot = slots_[reservation.index_];
        slot.loan = loan;
        slot.control.store(control(reservation.id_, SlotState::Producing), std::memory_order_release);
        auto result = ticket<false>(reservation.index_, reservation.id_, loan);
        reservation = {};
        if (enable_stats_) acquire_success_.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    bool cancel_reservation(QueueReservation& reservation, AcquireStatus reason) noexcept {
        if (!transition(reservation, SlotState::Reserved, SlotState::Releasing)) return false;
        free_slot(reservation.index_, reservation.id_);
        reservation = {};
        (void)failure(reason);
        return true;
    }
    bool send(ProducerLoan& producer) noexcept {
        if (!transition(producer, SlotState::Producing, SlotState::Publishing)) return false;
        publish(producer.index_, producer.id_);
        producer = {};
        return true;
    }
    [[nodiscard]] Loan try_receive() noexcept {
        std::uint32_t index;
        if (!ready_.try_pop(index)) return {};
        auto& slot = slots_[index];
        const auto id = slot.control.load(std::memory_order_relaxed) >> 4;
        auto loan = slot.loan;
        received_.fetch_add(1, std::memory_order_relaxed);
        // receive releases only B's capacity reservation; the loan remains active until complete_received.
        release_slot(index, id);
        return loan;
    }
    [[nodiscard]] Loan pop() noexcept { return try_receive(); }
    [[nodiscard]] Claim try_claim() noexcept {
        std::uint32_t index;
        if (!ready_.try_pop(index)) return {};
        auto& slot = slots_[index];
        const auto id = next_id(slot.control.load(std::memory_order_relaxed));
        auto loan = slot.loan;
        // Give every new claim a generation: cancelling must invalidate old Claims even for the same resource.
        slot.control.store(control(id, SlotState::Claimed), std::memory_order_release);
        return ticket<true>(index, id, loan);
    }
    bool cancel_claim(Claim& claim) noexcept {
        if (!transition(claim, SlotState::Claimed, SlotState::Publishing)) return false;
        slots_[claim.index_].control.store(control(claim.id_, SlotState::Ready), std::memory_order_release);
        if (!ready_.requeue(static_cast<std::uint32_t>(claim.index_))) std::terminate();
        claim = {};
        return true;
    }
    template<bool C> [[nodiscard]] Completion begin_finish(Ticket<C>& value) noexcept {
        if (!transition(value, C ? SlotState::Claimed : SlotState::Producing,
                        SlotState::Finishing)) return {};
        Completion result;
        result.owner_ = this;
        result.index_ = value.index_;
        result.id_ = value.id_;
        result.loan_ = slots_[value.index_].loan;
        value = {};
        return result;
    }
    bool complete_finish(Completion& completion) noexcept {
        // CAS only the stable slot's generation; do not read the generation of a possibly destroyed resource.
        if (!transition(completion, SlotState::Finishing, SlotState::Releasing)) return false;
        free_slot(completion.index_, completion.id_);
        completion = {};
        return true;
    }
    void complete_received() noexcept {
        if (received_.fetch_sub(1, std::memory_order_relaxed) == 0) std::terminate();
        leave();
    }
    void close() noexcept { active_.fetch_or(closed_bit, std::memory_order_acq_rel); }
    [[nodiscard]] bool is_closed() const noexcept {
        return (active_.load(std::memory_order_acquire) & closed_bit) != 0;
    }
    [[nodiscard]] bool is_drained() const noexcept {
        return active_.load(std::memory_order_acquire) == closed_bit;
    }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    [[nodiscard]] bool has_ready() const noexcept { return ready_.has_ready(); }
    // Read stable slots only; never dereference a ticket's resource address after that resource might have been destroyed.
    template<bool C> [[nodiscard]] HandoffState state(const Ticket<C>& value) const noexcept {
        if (value.owner_ != this || value.index_ >= slots_.size()) return HandoffState::Invalid;
        const auto current = slots_[value.index_].control.load(std::memory_order_acquire);
        if ((current >> 4) != value.id_) return HandoffState::Invalid;
        switch (static_cast<SlotState>(current & state_mask)) {
        case SlotState::Free: return HandoffState::Free;
        case SlotState::Reserved: case SlotState::Attaching: case SlotState::Producing:
            return HandoffState::Producing;
        case SlotState::Publishing: case SlotState::Ready: return HandoffState::Ready;
        case SlotState::Claimed: return HandoffState::Claimed;
        case SlotState::Finishing: case SlotState::Releasing: return HandoffState::Recycling;
        }
        return HandoffState::Invalid;
    }
    // Concurrent observations are snapshots; quiescent counts are exact. Scan on demand to avoid global-counter contention on each transition.
    [[nodiscard]] HandoffStats stats() const noexcept {
        HandoffStats result;
        result.queue_capacity = capacity();
        result.counters_enabled = enable_stats_;
        for (const auto& slot : slots_) {
            switch (static_cast<SlotState>(slot.control.load(std::memory_order_relaxed) & state_mask)) {
            case SlotState::Reserved: case SlotState::Attaching: case SlotState::Producing:
                ++result.producing_count; break;
            case SlotState::Publishing: case SlotState::Ready: ++result.ready_count; break;
            case SlotState::Claimed: ++result.claimed_count; break;
            case SlotState::Finishing: case SlotState::Releasing: ++result.finishing_count; break;
            default: break;
            }
        }
        result.received_count = received_.load(std::memory_order_relaxed);
        result.acquire_success = acquire_success_.load(std::memory_order_relaxed);
        result.queue_full = queue_full_.load(std::memory_order_relaxed);
        result.pool_limit = pool_limit_.load(std::memory_order_relaxed);
        result.resource_error = resource_error_.load(std::memory_order_relaxed);
        result.acquire_closed = acquire_closed_.load(std::memory_order_relaxed);
        return result;
    }

private:
    static std::size_t checked_capacity(std::size_t capacity) {
        if (!capacity || capacity > std::numeric_limits<std::uint32_t>::max() - 2)
            throw std::invalid_argument("queue_capacity must be in [1, UINT32_MAX - 2]");
        return capacity;
    }
    struct alignas(64) Slot {
        std::atomic<std::uint64_t> control{0};
        Loan loan;
    };
    static std::uint64_t control(std::uint64_t id, SlotState state) noexcept {
        return (id << 4) | static_cast<std::uint64_t>(state);
    }
    static std::uint64_t next_id(std::uint64_t old) noexcept {
        const auto id = (old >> 4) + 1;
        if (id > (std::numeric_limits<std::uint64_t>::max() >> 4)) std::terminate();
        return id;
    }
    ReserveResult failure(AcquireStatus status) noexcept {
        if (enable_stats_) {
            switch (status) {
            case AcquireStatus::QueueFull: queue_full_.fetch_add(1, std::memory_order_relaxed); break;
            case AcquireStatus::PoolLimit: pool_limit_.fetch_add(1, std::memory_order_relaxed); break;
            case AcquireStatus::ResourceError: resource_error_.fetch_add(1, std::memory_order_relaxed); break;
            case AcquireStatus::Closed: acquire_closed_.fetch_add(1, std::memory_order_relaxed); break;
            default: break;
            }
        }
        return {status, {}};
    }
    template<class T> bool transition(const T& value, SlotState from, SlotState to) noexcept {
        if (value.owner_ != this || value.index_ >= slots_.size()) return false;
        auto expected = control(value.id_, from);
        // Do not read slot.loan before winning CAS: an old ticket may race with attachment of another resource.
        return slots_[value.index_].control.compare_exchange_strong(expected, control(value.id_, to),
            std::memory_order_acq_rel, std::memory_order_relaxed);
    }
    template<bool C> Ticket<C> ticket(std::size_t index, std::uint64_t id, Loan loan) const noexcept {
        Ticket<C> result;
        result.owner_ = this;
        result.index_ = index;
        result.id_ = id;
        result.loan_ = loan;
        return result;
    }
    void publish(std::size_t index, std::uint64_t id) noexcept {
        auto& slot = slots_[index];
        slot.control.store(control(id, SlotState::Ready), std::memory_order_release);
        if (!ready_.try_push(static_cast<std::uint32_t>(index))) std::terminate();
    }
    void release_slot(std::size_t index, std::uint64_t id) noexcept {
        auto& slot = slots_[index];
        slot.loan = {};
        slot.control.store(control(id, SlotState::Free), std::memory_order_relaxed);
        free_.push(static_cast<std::uint32_t>(index));
    }
    void free_slot(std::size_t index, std::uint64_t id) noexcept {
        release_slot(index, id);
        leave();
    }
    void leave() noexcept { active_.fetch_sub(1, std::memory_order_release); }

    LockFreeIndexStack free_;
    ReadyIndexQueue ready_;
    std::vector<Slot> slots_;
    const bool enable_stats_;
    alignas(64) std::atomic<std::uint64_t> active_{0};
    alignas(64) std::atomic<std::size_t> received_{0};
    alignas(64) std::atomic<std::uint64_t> acquire_success_{0}, queue_full_{0}, pool_limit_{0},
        resource_error_{0}, acquire_closed_{0};
};

} // namespace TwoBufQueue
