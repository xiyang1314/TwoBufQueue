#pragma once
#include "TwoBufQueue/src/write/writer_base.hpp"
#include "TwoBufQueue/src/write/sync_context.hpp"

#include <algorithm>

namespace TwoBufQueue {

// Single-producer writer; TIsWriteLen selects whether records include a length prefix:
//   true: encode payload length in sizeof(TDataLen) little-endian bytes, then tightly copy object representations in argument order.
//         The prefix makes record boundaries self-describing and permits empty payloads.
//   false: tightly copy bytes without a prefix; boundaries are not self-describing, so empty payloads are rejected.
// Publication count is part of the type, not a constructor argument; one object never mixes the two policies.
// TSyncContext = void uses asynchronous buffers; non-void invokes the callback directly without these prefixes.
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
    // Ordinary asynchronous writes read back the first byte after copying; Session retains its write-only path.
    static constexpr bool ordinary_write_readback = !is_sync;
    using DataLen = TDataLen;
    static constexpr std::size_t message_prefix_size = TIsWriteLen ? sizeof(TDataLen) : 0;

    explicit Writer(WriterQueue* queue, std::int32_t thread_id,
                    WriterBackpressureConfig backpressure = {})
        : Base(queue, thread_id, message_prefix_size, backpressure) {}

    // Single-block Session: write checks capacity, copies and advances only; callers switch blocks with get_next_session.
    // Check empty objects with if (!session). Clearing/destruction synchronizes cursors without implicit flush or handoff.
    // Session lifetime must be shorter than Writer; do not operate Writer directly while Session is active.
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
        // Publish the current block's position and notify the reader without switching blocks; calling is optional.
        // With is_scan_current = false, get_next_session / finish_block / finish must also hand off the tail.
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


        // Source memory must be valid and nonoverlapping. Zero length is invalid; exceeding the whole block has a separate length error.
        [[nodiscard]] FAST_LOG_ALWAYS_INLINE SessionWriteResult write_mem(const void* data, TDataLen len) noexcept {
            if (FAST_LOG_UNLIKELY(len == 0)) return SessionWriteResult::invalid_argument;
            const auto size = static_cast<std::size_t>(len);
            if (FAST_LOG_UNLIKELY(size > static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) - message_prefix_size))
                return SessionWriteResult::exceeds_block_capacity;
            const auto result = state_.prepare(size + message_prefix_size);
            if (FAST_LOG_UNLIKELY(result != SessionWriteResult::success)) return result;
            assert(data != nullptr); // As with memcpy, the caller must provide valid source memory.
            std::byte* const begin = state_.cursor();
            if constexpr (TIsWriteLen) write_length(begin, len);
            std::memcpy(begin + message_prefix_size, data, size);
            state_.commit(begin + message_prefix_size + size);
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

    // Tightly copy object representations in argument order; only trivially copyable nonpointer, nonarray values.
    // With prefixes, encode the payload length in sizeof(TDataLen) little-endian bytes; without prefixes reject empty payloads.
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
            // Preserve an actual first-byte readback; commit_write subsequently publishes data with release semantics.
            (void)*reinterpret_cast<volatile std::byte*>(begin);
            this->commit_write(begin + message_prefix_size + size);
        }
        return true;
    }

    // Asynchronous mode copies len payload bytes; synchronous mode borrows them for this call without retaining data.
    // Source memory must be valid and, in asynchronous mode, must not overlap destination storage.
    // Like write, TIsWriteLen selects prefixes; TDataLen consistently represents lengths to prevent narrowing truncation.
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
        // Fixed-count loops may unroll; bytewise encoding is independent of host endianness and alignment.
        for (std::size_t i = 0; i < sizeof(TDataLen); ++i)
            target[i] = static_cast<std::byte>((len >> (i * 8)) & 255);
    }
};

}  // namespace TwoBufQueue
