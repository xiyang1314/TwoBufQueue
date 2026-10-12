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

// 将热路径提示、冷函数隔离和强制内联统一映射到各编译器。
// FAST_LOG_* 宏被 WriterBase/Writer 共用，因此只在此定义一次、
// 不再 #undef；宏名带 FAST_LOG_ 前缀，边界清晰。
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

// 仅用于由块生命周期保证的内部不变量，条件不得包含副作用。
#if defined(__clang__)
#define FAST_LOG_ASSUME(condition) __builtin_assume(!!(condition))
#elif defined(__GNUC__)
#define FAST_LOG_ASSUME(condition) do { if (!(condition)) __builtin_unreachable(); } while (false)
#elif defined(_MSC_VER)
#define FAST_LOG_ASSUME(condition) __assume(condition)
#else
#define FAST_LOG_ASSUME(condition) ((void)0)
#endif

// 单生产线程写入器的公共骨架：块管理、flush/finish、背压、保存侧读取与换块。
// 是否写长度前缀由派生类 Writer 的模板参数 TIsWriteLen 决定。
// 发布次数属于类型，构造函数不再接收次数，同一对象不会混用两种策略。
// uint32_t 最大值表示写入时只推进本地游标；仅 flush、换块和 finish 发布。
// 仅交接时发布的模式固定缓存剩余容量；逐条/批量发布保持原来的指针差检查。
template <std::uint32_t publish_write_ptr_count = 1>
class WriterBase {
    static_assert(publish_write_ptr_count > 0,
                  "Writer publish_write_ptr_count must be positive");
public:
    static constexpr bool session_enabled =
        publish_write_ptr_count == (std::numeric_limits<std::uint32_t>::max)();

    // 公共基类的前缀大小；Writer 在 TIsWriteLen=true 时以 sizeof(TDataLen) 覆盖。
    static constexpr std::size_t message_prefix_size = 0;

    [[nodiscard]] WriterQueue& handoff_queue() const noexcept { return *queue_; }
    [[nodiscard]] std::int32_t thread_id() const noexcept { return thread_id_; }
    // 查询占用，不改变写入/发布状态；上层可据此过滤数据。
    [[nodiscard]] bool usage_at_or_above(std::uint8_t percentage) const noexcept {
        if (!percentage) return false;
        const auto& pool = queue_->pool();
        const auto max = pool.max_count();
        if (percentage >= 100) return pool.in_use_count() >= max;
        return pool.in_use_count() >= (max / 100) * percentage + ((max % 100) * percentage + 99) / 100;
    }

    // 跨线程发布依赖无锁原子指针，不允许退化为内部互斥锁实现。
    static_assert(std::atomic<std::byte*>::is_always_lock_free,
                  "Writer requires lock-free atomic pointers");

    // 生产端/停止后的诊断快照，不保证跨线程解引用时的块寿命。
    // 消费端通过 handoff_queue().try_pop() 取得交接记录，从 record.next() 跟踪当前生产块。
    [[nodiscard]] SpscWriteBlock* source_block() const noexcept {
        return published_source_block_.load(std::memory_order_acquire);
    }
    // 当前块的发布位置快照；无当前块时返回空。
    // 同样只用于生产端或写入停止后的诊断；读取端从队列取块。
    [[nodiscard]] std::byte* published_write_ptr() const noexcept {
        SpscWriteBlock* const range = source_block();
        return range == nullptr ? nullptr :
            range->write_ptr_.load(std::memory_order_acquire);
    }

    // 逐项 relaxed 读取冷统计并组装为值快照。
    [[nodiscard]] WriterBackpressureStats backpressure_stats() const noexcept {
        return {pressure_events_.load(std::memory_order_relaxed), dropped_records_.load(std::memory_order_relaxed),
            blocked_events_.load(std::memory_order_relaxed), blocked_ticks_.load(std::memory_order_relaxed),
            max_blocked_ticks_.load(std::memory_order_relaxed)};
    }

protected:
    // 同步路径复用单写线程检查与 finish 状态，不申请/发布消息缓存。
    [[nodiscard]] FAST_LOG_ALWAYS_INLINE bool can_write_direct() noexcept {
        check_writer_thread();
        check_no_session();
        return !finished_;
    }

    // 外部队列和池的寿命必须覆盖 Writer 与读取器；等待/丢弃策略属于 Writer。
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

    // 设置最后一块为 WriteFinished 并交接；读取端完成 Claim 后归还。
    ~WriterBase() noexcept {
        finish();
    }

    // 写入器绑定线程私有游标和外部生命周期依赖，因此禁止复制。
    WriterBase(const WriterBase&) = delete;
    // 禁止复制赋值，避免两个对象同时写入和释放同一当前块。
    WriterBase& operator=(const WriterBase&) = delete;

#pragma region 写数据
public:
    // stop producing permanently and enqueue the final completion before stopping the reader.
    void finish() noexcept {
        check_no_session();
        if (finished_) return;
        finish_block();
        finished_ = true;
    }
    // 交接当前块但保留 Writer 可写状态；下一次 write 按正常路径申请新块。
    // 不增加消息、不等待消费；仅由写线程或在写线程停止后调用。
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
    // 公布当前块的全部已写数据，含未满发布次数的尾部；不完成块。
    // 由写入线程调用，或在写入停止后调用，不能与本对象的写入并发。
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
    // 一次 Session 只绑定一块；write 不调用 Writer，不取块、不发布、不重试。
    // 地址不保存到 Writer，也不传给冷函数，使游标/容量能留在寄存器。
    // Writer 必须比 Session 活得更久；活跃 Session 期间不得直接操作 Writer。
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
        // 转移独占的写入状态；原对象清空后，析构不会归还或覆盖新对象的游标。
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
            // 所有记录至少一字节；无效 Session 的 remaining_ 恒为 0。
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
            owner_->flush(); // 只发布位置，不交接、不换块，Session 仍可写。
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

    // 超时只作用于取块，独立于普通 write 的 Drop/Block 策略。
    // next 使用一次 {旧块,新块} 交接；失败时旧块也已交出，随后仅重试申请。
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

    // 两个入口共用空间检查；一次写入完整保留在一块内存中。
    FAST_LOG_ALWAYS_INLINE std::byte* prepare_write(std::size_t size) noexcept {
        check_writer_thread();
        check_no_session();
        if constexpr (session_enabled) {
            if (FAST_LOG_UNLIKELY(write_capacity_.remaining == 0 || size > write_capacity_.remaining)) {
                if (!switch_block_for_record(size)) return nullptr;
            }
            auto* begin = local_write_ptr_;
            write_capacity_.remaining -= size;
            // 工厂保证数据地址非空；失败的换块已在上面返回。
            // 避免调用者在成功路径中再次检查同一个返回地址。
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

    // 全部复制完成后才发布，读取侧不会看到半次写入。
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

    // 单值直接借用对象表示；多值保持与缓冲写入相同的紧密字节布局。
    // 临时字节数组仅在本次回调内有效，不含长度或 seq 前缀。
    template<class Callback, typename... Args>
    static FAST_LOG_ALWAYS_INLINE void with_argument_bytes(Callback&& callback, const Args&... args) noexcept {
        if constexpr (sizeof...(Args) == 1) {
            (callback(reinterpret_cast<const std::byte*>(std::addressof(args)), sizeof(Args)), ...);
        } else {
            constexpr std::size_t size = (std::size_t{0} + ... + sizeof(Args));
            std::array<std::byte, size == 0 ? 1 : size> payload;
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

    // 单写线程约束：首个调用 write 的线程即所有者，后续写入必须来自同一线程。
    // 仅调试构建启用；发布构建下函数体为空并被内联移除，零开销。
    FAST_LOG_ALWAYS_INLINE void check_writer_thread() noexcept {
#ifndef NDEBUG
        const auto current = std::this_thread::get_id();
        if (writer_thread_ == std::thread::id{}) writer_thread_ = current;
        assert(writer_thread_ == current);
#endif
    }

    // 公布当前块的数据位置；不完成块。
    void flush_immediate() noexcept {
        // 完成块已移交但替换块尚未取得时，没有当前块需要发布。
        if (source_block_ == nullptr) return;
        // 每次写入已经发布到当前块；保留 flush 的最终位置发布。
        std::byte* const published =
            source_block_->write_ptr_.load(std::memory_order_relaxed);
        source_block_->write_ptr_.store(published, std::memory_order_release);
    }

    // 不足一批的数据在 flush、换块和析构时也必须发布。
    void flush_batched() noexcept {
        if (source_block_ == nullptr) return;
        source_block_->write_ptr_.store(local_write_ptr_, std::memory_order_release);
        publish_countdown_ = publish_write_ptr_count;
    }

#pragma endregion

#pragma region 切换内存块
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
        // 旧块已经发布，读取端可以归还；单块池不会先等待再交接。
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

    // 换块后把 SpscWriteBlock 关键字段缓存为直接成员，避免逐条写入时多一次间接访问。
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

        // 所有新块相关字段就绪后，才以 release 发布范围指针给保存侧。
        published_source_block_.store(range, std::memory_order_release);
    }

    // 空间不足时移交旧块与下一块；背压时先提交旧块，恢复后再提交新块。
    [[nodiscard]] FAST_LOG_COLD_NOINLINE bool
    switch_block_for_record(std::uint64_t record_size) noexcept;

#pragma endregion

private:
    [[noreturn]] static void invalid_handoff(const char* message) noexcept {
        std::fprintf(stderr, "Writer: %s\n", message);
        std::terminate();
    }

    // 队列和线程编号在写入器生命周期内保持不变。
    WriterQueue* const queue_;
    const std::int32_t thread_id_;
    const WriterBackpressureConfig backpressure_;
    alignas(64) std::atomic<std::uint64_t> pressure_events_{0}, dropped_records_{0}, blocked_events_{0},
        blocked_ticks_{0}, max_blocked_ticks_{0};
    // 仅交接时发布的模式使用线程私有容量计数；其他发布模式保留原来的布局。
    struct WriteCapacity { std::size_t remaining = 0; };
    struct NoCapacity {};
    [[no_unique_address]] std::conditional_t<
        session_enabled,
        WriteCapacity, NoCapacity> write_capacity_;
    bool finished_ = false;
    // 只记录独占状态；不保存 Session 地址，避免本地写入状态逃逸。
    struct SessionFlag { bool active = false; };
    struct NoSessionFlag {};
    [[no_unique_address]] std::conditional_t<session_enabled, SessionFlag, NoSessionFlag> write_session_;
    // 单写线程所有者；由 check_writer_thread 在首次 write 时惰性绑定，仅调试构建校验。
    std::thread::id writer_thread_{};
    // 当前 SpscWriteBlock 的数据首地址，由 cache_range 在构造和换块时更新。
    std::byte* buf_ = nullptr;
    // 仅批量写入路径使用：真实终点可领先于块上已经发布的 write_ptr_。
    std::byte* local_write_ptr_ = nullptr;
    // 当前块尾后地址，用于一次减法完成剩余容量检查。
    std::byte* block_end_ = nullptr;
    // 当前块总字节数，冷路径用它拒绝大于完整块的记录。
    std::uint64_t buf_size_ = 0;
    // 仅成功写入才递减；发布、flush 和换块后重置。
    std::uint32_t publish_countdown_ = publish_write_ptr_count;

    // 生产线程直接使用的当前 SpscWriteBlock 指针，不承担跨线程发布职责。
    SpscWriteBlock* source_block_ = nullptr;
    // 所有新块缓存字段就绪后发布给扫描侧的当前范围指针。
    alignas(64) std::atomic<SpscWriteBlock*> published_source_block_{nullptr};
    // 仅允许测试辅助类访问内部游标和块生命周期状态。
    friend class WriterForTest;
};

// 类模板的冷函数定义必须对各实例可见；仍保留 noinline/cold 属性。
template <std::uint32_t publish_write_ptr_count>
bool WriterBase<publish_write_ptr_count>::switch_block_for_record(std::uint64_t record_size) noexcept {
    if (finished_ || record_size > buf_size_) [[unlikely]] return false;
    flush();
    SpscWriteBlock* const finished = source_block_;
    // 交出后不能再通过写入器访问旧块；acquire_next 可能阻塞等待其归还。
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
