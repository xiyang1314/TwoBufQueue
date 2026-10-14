#pragma once
#include "TwoBufQueue/src/utils/brqHelp.hpp"

#include "TwoBufQueue/src/write/make_queue.hpp"
#include "TwoBufQueue/src/write/types.hpp"

#include <array>
#include <cassert>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace TwoBufQueue {

class WriterForTest;

// Map hot-path hints, cold-function separation and forced inlining consistently across compilers.
// WriterBase/Writer share FAST_LOG_* macros, so define them once here
// without undefining them; FAST_LOG_ keeps the macro namespace explicit.
#if defined(__GNUC__) && !defined(__clang__)
#define FAST_LOG_UNLIKELY(condition) (__builtin_expect(!!(condition), 0))
#define FAST_LOG_COLD_NOINLINE __attribute__((cold, noinline, noclone))
#define FAST_LOG_ALWAYS_INLINE inline __attribute__((always_inline))
#define FAST_LOG_INLINE_LAMBDA __attribute__((always_inline))
#elif defined(__clang__)
#define FAST_LOG_UNLIKELY(condition) (__builtin_expect(!!(condition), 0))
#define FAST_LOG_COLD_NOINLINE __attribute__((cold, noinline))
#define FAST_LOG_ALWAYS_INLINE inline __attribute__((always_inline))
#define FAST_LOG_INLINE_LAMBDA __attribute__((always_inline))
#elif defined(_MSC_VER)
#define FAST_LOG_UNLIKELY(condition) (condition)
#define FAST_LOG_COLD_NOINLINE __declspec(noinline)
#define FAST_LOG_ALWAYS_INLINE __forceinline
#define FAST_LOG_INLINE_LAMBDA
#else
#define FAST_LOG_UNLIKELY(condition) (condition)
#define FAST_LOG_COLD_NOINLINE
#define FAST_LOG_ALWAYS_INLINE inline
#define FAST_LOG_INLINE_LAMBDA
#endif

// Internal invariants guaranteed by block lifetimes only; conditions must have no side effects.
#if defined(__clang__)
#define FAST_LOG_ASSUME(condition) __builtin_assume(!!(condition))
#elif defined(__GNUC__)
#define FAST_LOG_ASSUME(condition) do { if (!(condition)) __builtin_unreachable(); } while (false)
#elif defined(_MSC_VER)
#define FAST_LOG_ASSUME(condition) __assume(condition)
#else
#define FAST_LOG_ASSUME(condition) ((void)0)
#endif

// Shared single-producer writer skeleton: block management, flush/finish, backpressure, reader access and block switching.
// Derived Writer's TIsWriteLen template argument determines length-prefix encoding.
// Publication count is part of the type, not a constructor argument; one object never mixes the two policies.
// The uint32_t maximum advances only a local cursor during writes; publish on flush, block switch and finish only.
// Handoff-only publication caches remaining capacity; per-record/batched publication retains pointer-difference checks.
template <std::uint32_t publish_write_ptr_count = 1>
class WriterBase {
    static_assert(publish_write_ptr_count > 0,
                  "Writer publish_write_ptr_count must be positive");
public:
    static constexpr bool session_enabled =
        publish_write_ptr_count == (std::numeric_limits<std::uint32_t>::max)();

    // Shared base prefix size; Writer overrides it with sizeof(TDataLen) when TIsWriteLen = true.
    static constexpr std::size_t message_prefix_size = 0;

    [[nodiscard]] WriterQueue& handoff_queue() const noexcept { return *queue_; }
    [[nodiscard]] std::int32_t thread_id() const noexcept { return thread_id_; }
    // Query occupancy without changing write/publication state; higher layers may use it to filter data.
    [[nodiscard]] bool usage_at_or_above(std::uint8_t percentage) const noexcept {
        if (!percentage) return false;
        const auto& pool = queue_->pool();
        const auto max = pool.max_count();
        if (percentage >= 100) return pool.in_use_count() >= max;
        return pool.in_use_count() >= (max / 100) * percentage + ((max % 100) * percentage + 99) / 100;
    }

    // Cross-thread publication requires genuinely lock-free atomic pointers; no internal-mutex fallback is allowed.
    static_assert(std::atomic<std::byte*>::is_always_lock_free,
                  "Writer requires lock-free atomic pointers");

    // Diagnostic snapshot for the producer or after production stops; does not guarantee block lifetime for cross-thread dereference.
    // Consumers obtain handoff records through handoff_queue().try_pop() and follow record.next() for the current producer block.
    [[nodiscard]] SpscWriteBlock* source_block() const noexcept {
        return published_source_block_.load(std::memory_order_acquire);
    }
    // Published-position snapshot for current, or empty if no current block exists.
    // Also producer-only/post-stop diagnostics; readers obtain blocks through the queue.
    [[nodiscard]] std::byte* published_write_ptr() const noexcept {
        SpscWriteBlock* const range = source_block();
        return range == nullptr ? nullptr :
            range->write_ptr_.load(std::memory_order_acquire);
    }

    // Assemble a value snapshot with relaxed loads of individual cold-path statistics.
    [[nodiscard]] WriterBackpressureStats backpressure_stats() const noexcept {
        return {pressure_events_.load(std::memory_order_relaxed), dropped_records_.load(std::memory_order_relaxed),
            blocked_events_.load(std::memory_order_relaxed), blocked_ticks_.load(std::memory_order_relaxed),
            max_blocked_ticks_.load(std::memory_order_relaxed)};
    }

protected:
    // The synchronous path reuses producer-thread and finish-state checks without allocating/publishing message buffers.
    [[nodiscard]] FAST_LOG_ALWAYS_INLINE bool can_write_direct() noexcept {
        check_writer_thread();
        check_no_session();
        return !finished_;
    }

    // External queue/pool must outlive Writer and readers; Writer owns wait/drop policy.
    explicit WriterBase(WriterQueue* queue, std::int32_t thread_id,
                        std::size_t prefix_size, WriterBackpressureConfig backpressure)
        : queue_(queue), thread_id_(thread_id), backpressure_(backpressure) {
        if (!queue_) throw std::invalid_argument("writer queue must not be null");
        const auto& callbacks = queue_->callbacks();
        if (callbacks.on_send != writer_queue_callbacks.on_send ||
            callbacks.can_finish != writer_queue_callbacks.can_finish ||
            callbacks.on_finish != writer_queue_callbacks.on_finish)
            throw std::invalid_argument("writer requires writer_queue_callbacks (use make_writer_queue)");
        if (queue_->current_producer() || queue_->is_closed())
            throw std::invalid_argument("writer queue already has a producer or is closed");
        const auto capacity = queue_->pool().factory().block_size();
        if (prefix_size > capacity)
            throw std::invalid_argument("writer message prefix must fit its block");
        if (backpressure_.wait_get_timeout <= std::chrono::milliseconds::zero() ||
            backpressure_.wait_get_timeout > std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::duration::max()) ||
            (backpressure_.mode != BackpressureMode::Drop && backpressure_.mode != BackpressureMode::Block))
            throw std::invalid_argument("invalid writer backpressure configuration");
        auto* range = acquire_next(nullptr);
        if (!range) throw std::bad_alloc{};
        cache_range(range);
    }

    // Mark the final block WriteFinished and hand it off; the reader returns it after completing its Claim.
    ~WriterBase() noexcept {
        finish();
    }

    // Writer binds thread-local cursors and external lifetimes, so copying is forbidden.
    WriterBase(const WriterBase&) = delete;
    // Disable copy assignment so two writers cannot write/release the same current block.
    WriterBase& operator=(const WriterBase&) = delete;

#pragma region Write data
public:
    // stop producing permanently and enqueue the final completion before stopping the reader.
    void finish() noexcept {
        check_no_session();
        if (finished_) return;
        finish_block();
        finished_ = true;
    }
    // Hand off current without ending Writer; the next write acquires a block on the normal path.
    // Add no message and do not wait for consumption; call from the producer or after it stops.
    void finish_block() noexcept {
        check_no_session();
        if (source_block_ == nullptr) return;
        flush();
        source_block_ = nullptr;
        if constexpr (session_enabled)
            write_capacity_.remaining = 0;
        published_source_block_.store(nullptr, std::memory_order_release);
        queue_->send();
    }
    // Publish all written data in current, including a partial publication batch, without completing the block.
    // Call from the producer or after writing stops; never concurrently with writes on this object.
    void flush() noexcept {
        check_no_session();
        if (source_block_ == nullptr) return;
        if constexpr (publish_write_ptr_count == 1) {
            flush_immediate();
        } else {
            flush_batched();
        }
        queue_->notify_read_available();
    }

protected:
    // One Session binds one block; write never calls Writer, acquires blocks, publishes or retries.
    // Do not store its address in Writer or pass it to cold functions, allowing cursor/capacity to stay in registers.
    // Writer must outlive Session; never operate Writer directly while a Session is active.
    class LocalWriteState {
        WriterBase* owner_ = nullptr;
        std::byte* cursor_ = nullptr;
        std::size_t remaining_ = 0;
        std::size_t capacity_ = 0;
        static FAST_LOG_COLD_NOINLINE SessionWriteResult failure(
            bool valid, std::size_t size, std::size_t capacity) noexcept {
            if (!valid) return SessionWriteResult::error;
            return size > capacity ? SessionWriteResult::exceeds_block_capacity
                                   : SessionWriteResult::insufficient_space;
        }
    public:
        LocalWriteState() noexcept = default;
        explicit LocalWriteState(WriterBase& owner, bool next,
                                 std::chrono::nanoseconds timeout) noexcept {
            if constexpr (session_enabled) {
                if (!owner.acquire_session_block(next, timeout)) return;
                owner_ = &owner;
                cursor_ = owner.local_write_ptr_;
                capacity_ = owner.buf_size_;
                remaining_ = static_cast<std::size_t>(owner.block_end_ - cursor_);
                owner.write_session_.active = true;
            }
        }
        LocalWriteState(const LocalWriteState&) = delete;
        LocalWriteState& operator=(const LocalWriteState&) = delete;
        // Transfer exclusive write state; emptying the source ensures its destructor neither returns nor overwrites the new cursor.
        LocalWriteState(LocalWriteState&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)),
              cursor_(std::exchange(other.cursor_, nullptr)),
              remaining_(std::exchange(other.remaining_, 0)),
              capacity_(std::exchange(other.capacity_, 0)) {}
        LocalWriteState& operator=(LocalWriteState&& other) noexcept {
            if (this == &other) return *this;
            detach();
            owner_ = std::exchange(other.owner_, nullptr);
            cursor_ = std::exchange(other.cursor_, nullptr);
            remaining_ = std::exchange(other.remaining_, 0);
            capacity_ = std::exchange(other.capacity_, 0);
            return *this;
        }
        ~LocalWriteState() noexcept { detach(); }
        explicit operator bool() const noexcept { return owner_ != nullptr; }
        [[nodiscard]] FAST_LOG_ALWAYS_INLINE SessionWriteResult prepare(std::size_t size) noexcept {
            // Every record has at least one byte; invalid Sessions always have remaining_ = 0.
            if (FAST_LOG_UNLIKELY(size > remaining_))
                return failure(owner_ != nullptr, size, capacity_);
            remaining_ -= size;
            FAST_LOG_ASSUME(cursor_ != nullptr);
            return SessionWriteResult::success;
        }
        FAST_LOG_ALWAYS_INLINE std::byte* cursor() const noexcept { return cursor_; }
        FAST_LOG_ALWAYS_INLINE void commit(std::byte* end) noexcept { cursor_ = end; }
        void flush() noexcept {
            if (!owner_) return;
            synchronize();
            if constexpr (session_enabled) owner_->write_session_.active = false;
            owner_->flush(); // Publish the position without handing off/switching blocks; the Session stays writable.
            if constexpr (session_enabled) owner_->write_session_.active = true;
        }
    private:
        void detach() noexcept {
            if (!owner_) return;
            synchronize();
            if constexpr (session_enabled) owner_->write_session_.active = false;
            owner_ = nullptr;
            cursor_ = nullptr;
            remaining_ = capacity_ = 0;
        }
        void synchronize() noexcept {
            owner_->local_write_ptr_ = cursor_;
            if constexpr (session_enabled) owner_->write_capacity_.remaining = remaining_;
        }
    };

    // Timeout applies only to acquisition, independently of ordinary write's Drop/Block policy.
    // next publishes one {old, new} handoff; failure still hands off old, and subsequent attempts only retry acquisition.
    FAST_LOG_COLD_NOINLINE bool acquire_session_block(
        bool next, std::chrono::nanoseconds timeout) noexcept {
        if constexpr (!session_enabled) return false;
        else {
            if (write_session_.active || finished_ || timeout < std::chrono::nanoseconds::zero() ||
                queue_->is_closed()) return false;
            check_writer_thread();
            if (!next && source_block_) return true;
            const bool send = next && source_block_;
            if (send) {
                flush();
                source_block_ = nullptr;
                if constexpr (session_enabled) write_capacity_.remaining = 0;
                published_source_block_.store(nullptr, std::memory_order_release);
            }
            auto acquired = send ? queue_->send_and_acquire() : queue_->try_acquire();
            if (acquired) { cache_range(acquired.loan.get()); return true; }
            if (acquired.status != LiveAcquireStatus::PoolLimit &&
                acquired.status != LiveAcquireStatus::QueueFull) return false;
            pressure_events_.fetch_add(1, std::memory_order_relaxed);
            if (timeout == std::chrono::nanoseconds::zero()) return false;
            blocked_events_.fetch_add(1, std::memory_order_relaxed);
            const auto counter_begin = read_wait_counter();
            using Clock = std::chrono::steady_clock;
            const auto deadline = deadline_after<Clock>(Clock::now(), timeout);
            auto spins = backpressure_.spin_count;
            for (;;) {
                const auto now = Clock::now();
                if (now >= deadline) break;
                acquired = queue_->try_acquire();
                if (acquired) { cache_range(acquired.loan.get()); break; }
                if (acquired.status != LiveAcquireStatus::PoolLimit &&
                    acquired.status != LiveAcquireStatus::QueueFull) break;
                if (spins) { --spins; cpu_relax(); }
                else {
                    const auto retry = deadline_after<Clock>(now, std::chrono::milliseconds{1});
                    std::this_thread::sleep_until(retry < deadline ? retry : deadline);
                }
            }
            const auto elapsed = read_wait_counter() - counter_begin;
            blocked_ticks_.fetch_add(elapsed, std::memory_order_relaxed);
            auto previous = max_blocked_ticks_.load(std::memory_order_relaxed);
            while (previous < elapsed && !max_blocked_ticks_.compare_exchange_weak(
                previous, elapsed, std::memory_order_relaxed, std::memory_order_relaxed)) {}
            return source_block_ != nullptr;
        }
    }

    // Both entry points share the capacity check; each entire write stays in one block.
    FAST_LOG_ALWAYS_INLINE std::byte* prepare_write(std::size_t size) noexcept {
        check_writer_thread();
        check_no_session();
        if constexpr (session_enabled) {
            if (FAST_LOG_UNLIKELY(write_capacity_.remaining == 0 || size > write_capacity_.remaining)) {
                if (!switch_block_for_record(size)) return nullptr;
            }
            auto* begin = local_write_ptr_;
            write_capacity_.remaining -= size;
            // The Factory guarantees a non-null data address; failed block switches already returned above.
            // Avoid making the caller recheck the same returned address on success.
            FAST_LOG_ASSUME(begin != nullptr);
            return begin;
        } else {
            if (FAST_LOG_UNLIKELY(finished_)) return nullptr;
            if (FAST_LOG_UNLIKELY(source_block_ == nullptr)) {
                if (!switch_block_for_record(size)) return nullptr;
            }
            std::byte* begin;
            if constexpr (publish_write_ptr_count == 1) {
                begin = source_block_->write_ptr_.load(std::memory_order_relaxed);
            } else {
                begin = local_write_ptr_;
            }
            if (FAST_LOG_UNLIKELY(size > static_cast<std::size_t>(block_end_ - begin))) {
                if (!switch_block_for_record(size)) return nullptr;
                begin = buf_;
            }
            return begin;
        }
    }

    // Publish only after all copying finishes so readers never observe a partial write.
    FAST_LOG_ALWAYS_INLINE void commit_write(std::byte* end) noexcept {
        if constexpr (publish_write_ptr_count == 1) {
            source_block_->write_ptr_.store(end, std::memory_order_release);
        } else {
            local_write_ptr_ = end;
            if constexpr (publish_write_ptr_count != (std::numeric_limits<std::uint32_t>::max)()) {
                if (FAST_LOG_UNLIKELY(--publish_countdown_ == 0)) {
                    source_block_->write_ptr_.store(end, std::memory_order_release);
                    publish_countdown_ = publish_write_ptr_count;
                }
            }
        }
    }

    template <std::size_t... indices, typename... Args>
    static FAST_LOG_ALWAYS_INLINE void copy_arguments(
        std::index_sequence<indices...>, [[maybe_unused]] std::byte* begin, const Args&... args) noexcept {
        [[maybe_unused]] constexpr auto offsets = argument_offsets<Args...>();
        ((void)std::memcpy(begin + offsets[indices], std::addressof(args), sizeof(Args)), ...);
    }

    // Borrow one value's object representation directly; pack multiple values as tightly as buffered writes.
    // The temporary byte array is valid only during this callback and contains no length or seq prefixes.
    template<class Callback, typename... Args>
    static FAST_LOG_ALWAYS_INLINE void with_argument_bytes(Callback&& callback, const Args&... args) noexcept {
        if constexpr (sizeof...(Args) == 1) {
            (callback(reinterpret_cast<const std::byte*>(std::addressof(args)), sizeof(Args)), ...);
        } else if constexpr (sizeof...(Args) == 0) {
            // Keep a valid, initialized address for an empty synchronous payload.
            const std::byte empty{};
            callback(std::addressof(empty), std::size_t{0});
        } else {
            constexpr std::size_t size = (std::size_t{0} + ... + sizeof(Args));
            std::array<std::byte, size> payload;
            copy_arguments(std::index_sequence_for<Args...>{}, payload.data(), args...);
            callback(payload.data(), size);
        }
    }

    template <typename... Args>
    static consteval std::array<std::size_t, sizeof...(Args)> argument_offsets() noexcept {
        std::array<std::size_t, sizeof...(Args)> result{};
        std::size_t offset = 0;
        std::size_t index = 0;
        ((result[index++] = offset, offset += sizeof(Args)), ...);
        return result;
    }

private:
    FAST_LOG_ALWAYS_INLINE void check_no_session() const noexcept {
#ifndef NDEBUG
        if constexpr (session_enabled) assert(!write_session_.active);
#endif
    }

    // One-producer rule: the first thread calling write becomes owner; all later writes must use the same thread.
    // Debug builds only; the empty release-build body is inlined away without overhead.
    FAST_LOG_ALWAYS_INLINE void check_writer_thread() noexcept {
#ifndef NDEBUG
        const auto current = std::this_thread::get_id();
        if (writer_thread_ == std::thread::id{}) writer_thread_ = current;
        assert(writer_thread_ == current);
#endif
    }

    // Publish current's data position without completing the block.
    void flush_immediate() noexcept {
        // If a completed block was handed off but its replacement is not acquired, there is no current to publish.
        if (source_block_ == nullptr) return;
        // Every write already published to current; preserve flush's final position publication.
        std::byte* const published =
            source_block_->write_ptr_.load(std::memory_order_relaxed);
        source_block_->write_ptr_.store(published, std::memory_order_release);
    }

    // Publish a partial batch on flush, block switch and destruction too.
    void flush_batched() noexcept {
        if (source_block_ == nullptr) return;
        source_block_->write_ptr_.store(local_write_ptr_, std::memory_order_release);
        publish_countdown_ = publish_write_ptr_count;
    }

#pragma endregion

#pragma region Switch blocks
private:
    SpscWriteBlock* acquire_next(SpscWriteBlock* finished) noexcept {
        auto producer = queue_->current_producer();
        if (producer.get() != finished) invalid_handoff("finished block must equal current producer");
        auto acquired = producer ? queue_->send_and_acquire() : queue_->try_acquire();
        if (acquired) return acquired.loan.get();
        if (acquired.status == LiveAcquireStatus::Closed) return nullptr;
        if (acquired.status != LiveAcquireStatus::QueueFull && acquired.status != LiveAcquireStatus::PoolLimit &&
            acquired.status != LiveAcquireStatus::ResourceError) invalid_handoff("invalid source acquire state");
        pressure_events_.fetch_add(1, std::memory_order_relaxed);
        if (backpressure_.mode == BackpressureMode::Drop || !queue_->has_read_available_callback()) {
            dropped_records_.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        // The old block is published and may be returned by the reader; a one-block pool hands it off before waiting.
        blocked_events_.fetch_add(1, std::memory_order_relaxed);
        const auto begin = read_wait_counter();
        auto* next = wait_for_block();
        const auto elapsed = read_wait_counter() - begin;
        blocked_ticks_.fetch_add(elapsed, std::memory_order_relaxed);
        auto previous = max_blocked_ticks_.load(std::memory_order_relaxed);
        while (previous < elapsed && !max_blocked_ticks_.compare_exchange_weak(
            previous, elapsed, std::memory_order_relaxed, std::memory_order_relaxed)) {}
        if (!next) dropped_records_.fetch_add(1, std::memory_order_relaxed);
        return next;
    }
    SpscWriteBlock* wait_for_block() noexcept {
        using Clock = std::chrono::steady_clock;
        const auto deadline = deadline_after<Clock>(Clock::now(), backpressure_.wait_get_timeout);
        for (auto remaining = backpressure_.spin_count; remaining; --remaining) {
            if (auto result = queue_->try_acquire()) return result.loan.get();
            if (Clock::now() >= deadline) return nullptr;
            cpu_relax();
        }
        for (;;) {
            if (auto result = queue_->try_acquire()) return result.loan.get();
            const auto now = Clock::now();
            if (now >= deadline) return nullptr;
            const auto retry = deadline_after<Clock>(now, std::chrono::milliseconds{1});
            std::this_thread::sleep_until(retry < deadline ? retry : deadline);
        }
    }

    // Cache key SpscWriteBlock fields as direct members after switching to avoid an extra indirection per write.
    FAST_LOG_ALWAYS_INLINE void cache_range(SpscWriteBlock* range) noexcept {
        source_block_ = range;
        buf_ = range->buf_;
        block_end_ = buf_ + range->capacity();
        buf_size_ = range->capacity();
        if constexpr (session_enabled)
            write_capacity_.remaining = buf_size_;
        if constexpr (publish_write_ptr_count != 1) {
            local_write_ptr_ = buf_;
            publish_countdown_ = publish_write_ptr_count;
        }

        // Prepare all new-block fields before release-publishing the range pointer to the saving side.
        published_source_block_.store(range, std::memory_order_release);
    }

    // Hand off old/next when space runs out; under backpressure submit old first and publish new after recovery.
    [[nodiscard]] FAST_LOG_COLD_NOINLINE bool
    switch_block_for_record(std::uint64_t record_size) noexcept;

#pragma endregion

private:
    [[noreturn]] static void invalid_handoff(const char* message) noexcept {
        std::fprintf(stderr, "Writer: %s\n", message);
        std::terminate();
    }

    // Queue and source identifier stay constant throughout Writer's lifetime.
    WriterQueue* const queue_;
    const std::int32_t thread_id_;
    const WriterBackpressureConfig backpressure_;
    alignas(64) std::atomic<std::uint64_t> pressure_events_{0}, dropped_records_{0}, blocked_events_{0},
        blocked_ticks_{0}, max_blocked_ticks_{0};
    // Handoff-only publication uses a producer-private capacity counter; other modes retain the original layout.
    struct WriteCapacity { std::size_t remaining = 0; };
    struct NoCapacity {};
    [[no_unique_address]] std::conditional_t<
        session_enabled,
        WriteCapacity, NoCapacity> write_capacity_;
    bool finished_ = false;
    // Track exclusive state only; retain no Session address so local write state cannot escape.
    struct SessionFlag { bool active = false; };
    struct NoSessionFlag {};
    [[no_unique_address]] std::conditional_t<session_enabled, SessionFlag, NoSessionFlag> write_session_;
    // Single-producer owner; lazily bound on the first write by check_writer_thread and checked only in debug builds.
    std::thread::id writer_thread_{};
    // Current SpscWriteBlock data base, updated by cache_range at construction and block switches.
    std::byte* buf_ = nullptr;
    // Batched-write path only: the actual end may advance beyond the block's published write_ptr_.
    std::byte* local_write_ptr_ = nullptr;
    // One-past-current-block end, allowing remaining-capacity checks with one subtraction.
    std::byte* block_end_ = nullptr;
    // Current block capacity in bytes; the cold path rejects records larger than an entire block.
    std::uint64_t buf_size_ = 0;
    // Decrement only on successful writes; reset after publication, flush and block switches.
    std::uint32_t publish_countdown_ = publish_write_ptr_count;

    // Producer-private current SpscWriteBlock pointer; not responsible for cross-thread publication.
    SpscWriteBlock* source_block_ = nullptr;
    // Current range pointer published to scanners after every cached new-block field is ready.
    alignas(64) std::atomic<SpscWriteBlock*> published_source_block_{nullptr};
    // Internal cursors and block-lifecycle state are accessible only to the test helper.
    friend class WriterForTest;
};

// Template cold-function definitions must be visible to each instantiation; keep noinline/cold attributes.
template <std::uint32_t publish_write_ptr_count>
bool WriterBase<publish_write_ptr_count>::switch_block_for_record(std::uint64_t record_size) noexcept {
    if (finished_ || record_size > buf_size_) [[unlikely]] return false;
    flush();
    SpscWriteBlock* const finished = source_block_;
    // Do not access old through Writer after handoff; acquire_next may wait for its return.
    source_block_ = nullptr;
    if constexpr (session_enabled)
        write_capacity_.remaining = 0;
    published_source_block_.store(nullptr, std::memory_order_release);
    SpscWriteBlock* const next = acquire_next(finished);
    if (next == nullptr) [[unlikely]] return false;
    cache_range(next);
    return true;
}

}  // namespace TwoBufQueue
