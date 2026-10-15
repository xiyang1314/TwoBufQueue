#pragma once
#include "TwoBufQueue/src/write/writer_base.hpp"
#include "TwoBufQueue/src/write/sync_context.hpp"

#include <algorithm>

namespace TwoBufQueue {

// Single-producer writer; extends Writer by prepending a fixed-width little-endian sequence number to each record.
// Record layout: [seq (sizeof(TSeq), little-endian)] [length prefix, if TIsWriteLen] [payload].
// TSeq must be an unsigned integer of at most 64 bits; other template arguments match Writer.
// write / write_mem take a leading seq argument; seq bytes are not included in the payload length prefix.
// TSyncContext = void selects asynchronous buffering; a non-void context directly passes seq/payload without encoding prefixes.
template <typename TSeq, std::uint32_t TPublishWritePtrCount = 1,
          bool TIsWriteLen = true,
          typename TDataLen = std::uint16_t,
          class TSyncContext = void>
class SeqWriter : public WriterBase<TPublishWritePtrCount>,
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
    using Base = WriterBase<TPublishWritePtrCount>;
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

    // Single-block Session: write checks capacity, copies and advances only; callers switch blocks with get_next_session.
    // Check empty objects with if (!session). Clearing/destruction synchronizes cursors without implicit flush or handoff.
    // Session lifetime must be shorter than Writer; do not operate Writer directly while Session is active.
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
        // Publish the current block's position and notify the reader without switching blocks; calling is optional.
        // With is_scan_current = false, get_next_session / finish_block / finish must also hand off the tail.
        void flush() noexcept { state_.flush(); }
        template <typename... TArgs>
        [[nodiscard]] FAST_LOG_ALWAYS_INLINE SessionWriteResult write(TSeq seq, const TArgs&... args) noexcept {
            static_assert((std::is_trivially_copyable_v<TArgs> && ...),
                          "write requires trivially-copyable arguments");
            static_assert(((!std::is_pointer_v<TArgs> && !std::is_array_v<TArgs>) && ...),
                          "use write_mem for pointer/array payloads");
            constexpr std::size_t size = (std::size_t{0} + ... + sizeof(TArgs));
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
            Base::copy_arguments(std::index_sequence_for<TArgs...>{}, begin + total_prefix_size, args...);
            state_.commit(begin + total_prefix_size + size);

            return SessionWriteResult::success;
        }


        // Source memory must be valid and nonoverlapping. Zero length is invalid; exceeding the whole block has a separate length error.
        [[nodiscard]] FAST_LOG_ALWAYS_INLINE SessionWriteResult write_mem(TSeq seq, const void* data, TDataLen len) noexcept {
            if (FAST_LOG_UNLIKELY(len == 0)) return SessionWriteResult::invalid_argument;
            const auto size = static_cast<std::size_t>(len);
            if (FAST_LOG_UNLIKELY(size > static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) - total_prefix_size))
                return SessionWriteResult::exceeds_block_capacity;
            const auto result = state_.prepare(size + total_prefix_size);
            if (FAST_LOG_UNLIKELY(result != SessionWriteResult::success)) return result;
            assert(data != nullptr); // As with memcpy, the caller must provide valid source memory.
            std::byte* const begin = state_.cursor();
            write_seq(begin, seq);
            if constexpr (TIsWriteLen) write_length(begin + seq_size, len);
            std::memcpy(begin + total_prefix_size, data, size);
            state_.commit(begin + total_prefix_size + size);
            return SessionWriteResult::success;
        }
    };
    // Reuse current; wait for a block only when none exists. Zero tries once; positive timeouts retry until expiry.
    [[nodiscard]] WriteSession get_current_session(
        std::chrono::nanoseconds timeout = std::chrono::nanoseconds::zero()) noexcept
        requires (!is_sync && Base::session_enabled) { return WriteSession(*this, false, timeout); }
    // Hand off current even if partially full, then wait for another block. Clear or destroy the old Session first.
    // Reusing a variable: session = {}; session = writer.get_next_session(timeout);
    [[nodiscard]] WriteSession get_next_session(
        std::chrono::nanoseconds timeout = std::chrono::nanoseconds::zero()) noexcept
        requires (!is_sync && Base::session_enabled) { return WriteSession(*this, true, timeout); }

    // Copy object representations tightly in argument order, with seq first; only trivially copyable nonpointer, nonarray values.
    template <typename... TArgs>
    [[nodiscard]] FAST_LOG_ALWAYS_INLINE bool write(TSeq seq, const TArgs&... args) noexcept {
        static_assert((std::is_trivially_copyable_v<TArgs> && ...),
                      "write requires trivially-copyable arguments");
        static_assert(((!std::is_pointer_v<TArgs> && !std::is_array_v<TArgs>) && ...),
                      "use write_mem for pointer/array payloads");
        constexpr std::size_t size = (std::size_t{0} + ... + sizeof(TArgs));
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
            this->copy_arguments(std::index_sequence_for<TArgs...>{}, begin + total_prefix_size, args...);
            this->commit_write(begin + total_prefix_size + size);
        }
        return true;
    }

    // Asynchronous mode copies seq plus len payload bytes; synchronous mode borrows payload and passes seq separately without retaining data.
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
