#pragma once
#include "TwoBufQueue/src/write/writer_base.hpp"
#include "TwoBufQueue/src/write/sync_context.hpp"

#include <algorithm>

namespace TwoBufQueue {

// 单生产线程写入器；用 TIsWriteLen 决定是否写长度前缀：
//   TIsWriteLen = true  → 先写 sizeof(TDataLen) 字节的小端负载长度，再按参数顺序紧密复制对象表示。
//                         前缀使每条记录自描述边界，允许空负载。
//   TIsWriteLen = false → 不写前缀，直接紧密复制；无自描述边界，故拒绝空负载。
// 发布次数属于类型，构造函数不再接收次数，同一对象不会混用两种策略。
// TSyncContext=void 使用异步缓存；非 void 时直接回调，不写上述前缀。
template <std::uint32_t publish_write_ptr_count = 1,
          bool TIsWriteLen = true, typename TDataLen = std::uint16_t,
          class TSyncContext = void>
class Writer : public WriterBase<publish_write_ptr_count>,
               public detail::SyncBinding<TSyncContext> {
    static_assert(std::is_integral_v<TDataLen> && std::is_unsigned_v<TDataLen> &&
                  !std::is_same_v<TDataLen, bool> &&
                  std::is_same_v<TDataLen, std::remove_cv_t<TDataLen>> &&
                  sizeof(TDataLen) <= sizeof(std::uint64_t),
                  "TDataLen must be an unqualified unsigned integer of at most 64 bits");
    using Base = WriterBase<publish_write_ptr_count>;
public:
    static constexpr bool is_sync = !std::is_void_v<TSyncContext>;
    // 普通异步写入复制后读回首字节；Session 保持原来的纯写入路径。
    static constexpr bool ordinary_write_readback = !is_sync;
    using DataLen = TDataLen;
    static constexpr std::size_t message_prefix_size = TIsWriteLen ? sizeof(TDataLen) : 0;

    explicit Writer(WriterQueue* queue, std::int32_t thread_id,
                    WriterBackpressureConfig backpressure = {})
        : Base(queue, thread_id, message_prefix_size, backpressure) {}

    // 单块 Session：write 只检查容量、复制和推进；用户通过 get_next_session 换块。
    // 空对象用 if (!session) 检查。置空/析构只同步游标，不隐式 flush 或交接。
    // 生命周期必须短于 Writer；期间不得直接操作 Writer。
    class WriteSession {
        typename Base::LocalWriteState state_;
        friend class Writer;
        explicit WriteSession(Writer& writer, bool next, std::chrono::nanoseconds timeout) noexcept
            : state_(writer, next, timeout) {}
    public:
        WriteSession() noexcept = default;
        WriteSession(const WriteSession&) = delete;
        WriteSession& operator=(const WriteSession&) = delete;
        WriteSession(WriteSession&&) noexcept = default;
        WriteSession& operator=(WriteSession&&) noexcept = default;
        explicit operator bool() const noexcept { return static_cast<bool>(state_); }
        // 发布当前块的位置并通知读端；不切换块。用户自行决定是否调用。
        // is_scan_current=false 时，尾块还须 get_next_session/finish_block/finish 交接。
        void flush() noexcept { state_.flush(); }
        template <typename... Args>
        [[nodiscard]] FAST_LOG_ALWAYS_INLINE SessionWriteResult write(const Args&... args) noexcept {
            static_assert((std::is_trivially_copyable_v<Args> && ...),
                          "write requires trivially-copyable arguments");
            static_assert(((!std::is_pointer_v<Args> && !std::is_array_v<Args>) && ...),
                          "use write_mem for pointer/array payloads");
            constexpr std::size_t size = (std::size_t{0} + ... + sizeof(Args));
            if constexpr (TIsWriteLen) {
                static_assert(size <= (std::numeric_limits<TDataLen>::max)(),
                              "payload does not fit TDataLen");
                static_assert(size <= static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) - message_prefix_size,
                              "record size exceeds the addressable range");
            } else {
                static_assert(size != 0, "unframed payload must not be empty");
                static_assert(size <= static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()),
                              "record size exceeds the addressable range");
            }
            const auto result = state_.prepare(size + message_prefix_size);
            if (FAST_LOG_UNLIKELY(result != SessionWriteResult::success)) return result;
            std::byte* const begin = state_.cursor();
            if constexpr (TIsWriteLen) write_length(begin, static_cast<TDataLen>(size));
            Base::copy_arguments(std::index_sequence_for<Args...>{}, begin + message_prefix_size, args...);
            state_.commit(begin + message_prefix_size + size);
            return SessionWriteResult::success;
        }


        // 源内存必须有效且不与目标重叠。0 长度为参数错误；超出整块为专门的长度错误。
        [[nodiscard]] FAST_LOG_ALWAYS_INLINE SessionWriteResult write_mem(const void* data, TDataLen len) noexcept {
            if (FAST_LOG_UNLIKELY(len == 0)) return SessionWriteResult::invalid_argument;
            const auto size = static_cast<std::size_t>(len);
            if (FAST_LOG_UNLIKELY(size > static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) - message_prefix_size))
                return SessionWriteResult::exceeds_block_capacity;
            const auto result = state_.prepare(size + message_prefix_size);
            if (FAST_LOG_UNLIKELY(result != SessionWriteResult::success)) return result;
            assert(data != nullptr); // 与 memcpy 一样，有效源内存由调用方保证。
            std::byte* const begin = state_.cursor();
            if constexpr (TIsWriteLen) write_length(begin, len);
            std::memcpy(begin + message_prefix_size, data, size);
            state_.commit(begin + message_prefix_size + size);
            return SessionWriteResult::success;
        }
    };
    // 复用当前块；没有当前块时才等待取块。0 只尝试一次，正值重试到超时。
    [[nodiscard]] WriteSession get_current_session(
        std::chrono::nanoseconds timeout = std::chrono::nanoseconds::zero()) noexcept
        requires (!is_sync && Base::session_enabled) { return WriteSession(*this, false, timeout); }
    // 交接当前块（可未满），然后等待下一块。调用前须置空或销毁旧 Session。
    // 复用变量时：session = {}; session = writer.get_next_session(timeout);
    [[nodiscard]] WriteSession get_next_session(
        std::chrono::nanoseconds timeout = std::chrono::nanoseconds::zero()) noexcept
        requires (!is_sync && Base::session_enabled) { return WriteSession(*this, true, timeout); }

    // 按参数顺序紧密复制对象表示；仅支持可平凡复制的非指针、非数组值。
    // 有前缀时先写 sizeof(TDataLen) 字节的小端负载长度；无前缀时拒绝空负载。
    template <typename... Args>
    [[nodiscard]] FAST_LOG_ALWAYS_INLINE bool write(const Args&... args) noexcept {
        static_assert((std::is_trivially_copyable_v<Args> && ...),
                      "write requires trivially-copyable arguments");
        static_assert(((!std::is_pointer_v<Args> && !std::is_array_v<Args>) && ...),
                      "use write_mem for pointer/array payloads");
        constexpr std::size_t size = (std::size_t{0} + ... + sizeof(Args));
        if constexpr (TIsWriteLen) {
            static_assert(size <= (std::numeric_limits<TDataLen>::max)(),
                          "payload does not fit TDataLen");
            static_assert(size <= static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) - message_prefix_size,
                          "record size exceeds the addressable range");
        } else {
            static_assert(size != 0, "unframed payload must not be empty");
            static_assert(size <= static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()),
                          "record size exceeds the addressable range");
        }
        if constexpr (is_sync) {
            if (FAST_LOG_UNLIKELY(!this->can_write_direct() || !this->sync_context_)) return false;
            this->with_argument_bytes([&](const std::byte* data, std::size_t length) noexcept {
                this->sync_context_->invoke(this->thread_id(), data, length);
            }, args...);
        } else {
            std::byte* const begin = this->prepare_write(size + message_prefix_size);
            if (FAST_LOG_UNLIKELY(begin == nullptr)) return false;
            if constexpr (TIsWriteLen) write_length(begin, static_cast<TDataLen>(size));
            this->copy_arguments(std::index_sequence_for<Args...>{}, begin + message_prefix_size, args...);
            // 保留真实的首字节读回，随后仍由 commit_write 的 release 发布数据。
            (void)*reinterpret_cast<volatile std::byte*>(begin);
            this->commit_write(begin + message_prefix_size + size);
        }
        return true;
    }

    // 异步复制 len 字节负载；同步在本次调用内直接借用，不保存 data 指针。
    // 源内存必须有效；异步模式不得与目标重叠。
    // 与 write 一样用 TIsWriteLen 决定是否写长度前缀；长度统一使用 TDataLen，避免窄化截断。
    [[nodiscard]] FAST_LOG_ALWAYS_INLINE bool write_mem(const void* data, TDataLen len) noexcept {
        const auto size = static_cast<std::size_t>(len);
        if constexpr (is_sync) {
            if (FAST_LOG_UNLIKELY((len && !data) ||
                len > static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) - message_prefix_size)) return false;
            if constexpr (!TIsWriteLen) { if (len == 0) return false; }
            if (FAST_LOG_UNLIKELY(!this->can_write_direct() || !this->sync_context_)) return false;
            this->sync_context_->invoke(this->thread_id(), reinterpret_cast<const std::byte*>(data), size);
            return true;
        } else {
            if constexpr (TIsWriteLen) {
                if (FAST_LOG_UNLIKELY(len > static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) - message_prefix_size ||
                    (len && !data))) return false;
                std::byte* const begin = this->prepare_write(size + message_prefix_size);
                if (FAST_LOG_UNLIKELY(begin == nullptr)) return false;
                write_length(begin, len);
                if (size) std::copy_n(reinterpret_cast<const std::byte*>(data), size, begin + message_prefix_size);
                (void)*reinterpret_cast<volatile std::byte*>(begin);
                this->commit_write(begin + message_prefix_size + size);
            } else {
                if (FAST_LOG_UNLIKELY(len == 0 ||
                    len > static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) ||
                    (len && !data))) return false;
                std::byte* const begin = this->prepare_write(size);
                if (FAST_LOG_UNLIKELY(begin == nullptr)) return false;
                if (size) std::memcpy(begin, data, size);
                (void)*reinterpret_cast<volatile std::byte*>(begin);
                this->commit_write(begin + size);
            }
            return true;
        }
    }

private:
    static void write_length(std::byte* target, TDataLen len) noexcept {
        // 固定次数循环可展开；逐字节编码不依赖主机字节序或地址对齐。
        for (std::size_t i = 0; i < sizeof(TDataLen); ++i)
            target[i] = static_cast<std::byte>((len >> (i * 8)) & 255);
    }
};

}  // namespace TwoBufQueue
