#pragma once
#include "TwoBufQueue/src/write/writer_base.hpp"
#include "TwoBufQueue/src/write/sync_context.hpp"

#include <algorithm>

namespace TwoBufQueue {

// 单生产线程写入器；在 Writer 的基础上，每条记录最前面多写一个固定大小的小端 seq。
// 记录布局：[seq (sizeof(TSeq) 字节, 小端)] [长度前缀 (若 TIsWriteLen)] [负载]
// TSeq 是序号类型，必须是 ≤64 位的无符号整数；其余模板参数与 Writer 相同。
// write / write_mem 都比 Writer 多一个首位参数 seq，且 seq 不参与长度前缀计数。
// TSyncContext=void 使用异步缓存；非 void 时直接传递 seq 与负载，不编码前缀。
template <typename TSeq, std::uint32_t publish_write_ptr_count = 1,
          bool TIsWriteLen = true,
          typename TDataLen = std::uint16_t,
          class TSyncContext = void>
class SeqWriter : public WriterBase<publish_write_ptr_count>,
                  public detail::SyncBinding<TSyncContext> {
    static_assert(std::is_integral_v<TSeq> && std::is_unsigned_v<TSeq> &&
                  !std::is_same_v<TSeq, bool> &&
                  std::is_same_v<TSeq, std::remove_cv_t<TSeq>> &&
                  sizeof(TSeq) <= sizeof(std::uint64_t),
                  "TSeq must be an unqualified unsigned integer of at most 64 bits");
    static_assert(std::is_integral_v<TDataLen> && std::is_unsigned_v<TDataLen> &&
                  !std::is_same_v<TDataLen, bool> &&
                  std::is_same_v<TDataLen, std::remove_cv_t<TDataLen>> &&
                  sizeof(TDataLen) <= sizeof(std::uint64_t),
                  "TDataLen must be an unqualified unsigned integer of at most 64 bits");
    using Base = WriterBase<publish_write_ptr_count>;
public:
    static constexpr bool is_sync = !std::is_void_v<TSyncContext>;
    using SeqType = TSeq;
    using DataLen = TDataLen;
    static constexpr std::size_t seq_size = sizeof(TSeq);
    static constexpr std::size_t message_prefix_size = TIsWriteLen ? sizeof(TDataLen) : 0;
    static constexpr std::size_t total_prefix_size = seq_size + message_prefix_size;

    explicit SeqWriter(WriterQueue* queue, std::int32_t thread_id,
                       WriterBackpressureConfig backpressure = {})
        : Base(queue, thread_id, total_prefix_size, backpressure) {}

    // 单块 Session：write 只检查容量、复制和推进；用户通过 get_next_session 换块。
    // 空对象用 if (!session) 检查。置空/析构只同步游标，不隐式 flush 或交接。
    // 生命周期必须短于 Writer；期间不得直接操作 Writer。
    class WriteSession {
        typename Base::LocalWriteState state_;
        friend class SeqWriter;
        explicit WriteSession(SeqWriter& writer, bool next, std::chrono::nanoseconds timeout) noexcept
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
        [[nodiscard]] FAST_LOG_ALWAYS_INLINE SessionWriteResult write(TSeq seq, const Args&... args) noexcept {
            static_assert((std::is_trivially_copyable_v<Args> && ...),
                          "write requires trivially-copyable arguments");
            static_assert(((!std::is_pointer_v<Args> && !std::is_array_v<Args>) && ...),
                          "use write_mem for pointer/array payloads");
            constexpr std::size_t size = (std::size_t{0} + ... + sizeof(Args));
            if constexpr (TIsWriteLen) {
                static_assert(size <= (std::numeric_limits<TDataLen>::max)(),
                              "payload does not fit TDataLen");
                static_assert(size <= static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) - total_prefix_size,
                              "record size exceeds the addressable range");
            } else {
                static_assert(size != 0, "unframed payload must not be empty");
                static_assert(size <= static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) - seq_size,
                              "record size exceeds the addressable range");
            }
            const auto result = state_.prepare(size + total_prefix_size);
            if (FAST_LOG_UNLIKELY(result != SessionWriteResult::success)) return result;
            std::byte* const begin = state_.cursor();
            write_seq(begin, seq);
            if constexpr (TIsWriteLen) write_length(begin + seq_size, static_cast<TDataLen>(size));
            Base::copy_arguments(std::index_sequence_for<Args...>{}, begin + total_prefix_size, args...);
            state_.commit(begin + total_prefix_size + size);

            return SessionWriteResult::success;
        }


        // 源内存必须有效且不与目标重叠。0 长度为参数错误；超出整块为专门的长度错误。
        [[nodiscard]] FAST_LOG_ALWAYS_INLINE SessionWriteResult write_mem(TSeq seq, const void* data, TDataLen len) noexcept {
            if (FAST_LOG_UNLIKELY(len == 0)) return SessionWriteResult::invalid_argument;
            const auto size = static_cast<std::size_t>(len);
            if (FAST_LOG_UNLIKELY(size > static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) - total_prefix_size))
                return SessionWriteResult::exceeds_block_capacity;
            const auto result = state_.prepare(size + total_prefix_size);
            if (FAST_LOG_UNLIKELY(result != SessionWriteResult::success)) return result;
            assert(data != nullptr); // 与 memcpy 一样，有效源内存由调用方保证。
            std::byte* const begin = state_.cursor();
            write_seq(begin, seq);
            if constexpr (TIsWriteLen) write_length(begin + seq_size, len);
            std::memcpy(begin + total_prefix_size, data, size);
            state_.commit(begin + total_prefix_size + size);
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

    // 按参数顺序紧密复制对象表示，最前面写 seq；仅支持可平凡复制的非指针、非数组值。
    template <typename... Args>
    [[nodiscard]] FAST_LOG_ALWAYS_INLINE bool write(TSeq seq, const Args&... args) noexcept {
        static_assert((std::is_trivially_copyable_v<Args> && ...),
                      "write requires trivially-copyable arguments");
        static_assert(((!std::is_pointer_v<Args> && !std::is_array_v<Args>) && ...),
                      "use write_mem for pointer/array payloads");
        constexpr std::size_t size = (std::size_t{0} + ... + sizeof(Args));
        if constexpr (TIsWriteLen) {
            static_assert(size <= (std::numeric_limits<TDataLen>::max)(),
                          "payload does not fit TDataLen");
            static_assert(size <= static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) - total_prefix_size,
                          "record size exceeds the addressable range");
        } else {
            static_assert(size != 0, "unframed payload must not be empty");
            static_assert(size <= static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) - seq_size,
                          "record size exceeds the addressable range");
        }
        if constexpr (is_sync) {
            if (FAST_LOG_UNLIKELY(!this->can_write_direct() || !this->sync_context_)) return false;
            this->with_argument_bytes([&](const std::byte* data, std::size_t length) noexcept {
                this->sync_context_->invoke(this->thread_id(), seq, data, length);
            }, args...);
        } else {
            std::byte* const begin = this->prepare_write(size + total_prefix_size);
            if (FAST_LOG_UNLIKELY(begin == nullptr)) return false;
            write_seq(begin, seq);
            if constexpr (TIsWriteLen) write_length(begin + seq_size, static_cast<TDataLen>(size));
            this->copy_arguments(std::index_sequence_for<Args...>{}, begin + total_prefix_size, args...);
            this->commit_write(begin + total_prefix_size + size);
        }
        return true;
    }

    // 异步写 seq + len 字节负载；同步直接借用负载并单独传 seq，不保存 data 指针。
    [[nodiscard]] FAST_LOG_ALWAYS_INLINE bool write_mem(TSeq seq, const void* data, TDataLen len) noexcept {
        const auto size = static_cast<std::size_t>(len);
        if constexpr (is_sync) {
            if (FAST_LOG_UNLIKELY((len && !data) ||
                len > static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) - total_prefix_size)) return false;
            if constexpr (!TIsWriteLen) { if (len == 0) return false; }
            if (FAST_LOG_UNLIKELY(!this->can_write_direct() || !this->sync_context_)) return false;
            this->sync_context_->invoke(this->thread_id(), seq, reinterpret_cast<const std::byte*>(data), size);
            return true;
        } else {
            if constexpr (TIsWriteLen) {
                if (FAST_LOG_UNLIKELY(len > static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) - total_prefix_size ||
                    (len && !data))) return false;
                std::byte* const begin = this->prepare_write(size + total_prefix_size);
                if (FAST_LOG_UNLIKELY(begin == nullptr)) return false;
                write_seq(begin, seq);
                write_length(begin + seq_size, len);
                if (size) std::copy_n(reinterpret_cast<const std::byte*>(data), size, begin + total_prefix_size);
                this->commit_write(begin + total_prefix_size + size);
            } else {
                if (FAST_LOG_UNLIKELY(len == 0 ||
                    len > static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) - seq_size ||
                    (len && !data))) return false;
                std::byte* const begin = this->prepare_write(size + seq_size);
                if (FAST_LOG_UNLIKELY(begin == nullptr)) return false;
                write_seq(begin, seq);
                if (size) std::memcpy(begin + seq_size, data, size);
                this->commit_write(begin + seq_size + size);
            }
            return true;
        }
    }

private:
    static void write_seq(std::byte* target, TSeq seq) noexcept {
        for (std::size_t i = 0; i < sizeof(TSeq); ++i)
            target[i] = static_cast<std::byte>((seq >> (i * 8)) & 255);
    }
    static void write_length(std::byte* target, TDataLen len) noexcept {
        for (std::size_t i = 0; i < sizeof(TDataLen); ++i)
            target[i] = static_cast<std::byte>((len >> (i * 8)) & 255);
    }
};

}  // namespace TwoBufQueue
