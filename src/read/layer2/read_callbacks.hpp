#pragma once

#include "read_block.hpp"
#include <functional>
#include <type_traits>
#include <utility>

namespace TwoBufQueue {

// Match the Writer's DataLen. Each record starts with a little-endian payload length excluding the prefix; empty payloads are allowed.
// Invoking callback acknowledges one record; ignore its return value. The first argument, thread_id, identifies the producer source.
// Progress belongs to Block, so another thread retrying a cancelled claim does not repeat acknowledged records.
template<class TDataLen, class TCallback>
bool read_records(ReadBlock& block, TCallback& callback) noexcept {
    static_assert(std::is_integral_v<TDataLen> && std::is_unsigned_v<TDataLen> &&
                  !std::is_same_v<TDataLen, bool> &&
                  std::is_same_v<TDataLen, std::remove_cv_t<TDataLen>> && sizeof(TDataLen) <= sizeof(std::uint64_t),
                  "DataLen must match Writer's unsigned length type");
    static_assert(std::is_nothrow_invocable_r_v<void, TCallback&, std::int32_t, const std::byte*, std::size_t>,
                  "record callback must accept (thread_id, data, size) and be noexcept");
    if (block.record_error_ != ReadRecordError::none) return false;
    const auto batch = to_data_batch(block);
    while (block.record_frame_index_ < batch.frames.size()) {
        const auto& frame = batch.frames[block.record_frame_index_];
        while (block.record_offset_ < frame.size) {
            const auto remaining = frame.size - block.record_offset_;
            if (remaining < sizeof(TDataLen)) {
                block.record_error_ = ReadRecordError::incomplete_length;
                return false;
            }
            const auto* const prefix = frame.data + block.record_offset_;
            std::uint64_t length = 0;
            for (std::size_t i = 0; i < sizeof(TDataLen); ++i)
                length |= std::uint64_t{std::to_integer<unsigned char>(prefix[i])} << (i * 8);
            if (length > remaining - sizeof(TDataLen)) {
                block.record_error_ = ReadRecordError::incomplete_payload;
                return false;
            }
            const auto size = static_cast<std::size_t>(length);
            std::invoke(callback, frame.thread_id, prefix + sizeof(TDataLen), size);
            block.record_offset_ += sizeof(TDataLen) + size;
        }
        ++block.record_frame_index_;
        block.record_offset_ = 0;
    }
    return true;
}

// true accepts the whole batch; false rejects it. Do not produce partial effects that cannot safely be retried.
template<class TCallback>
auto make_batch_callback(TCallback callback) {
    static_assert(std::is_nothrow_invocable_r_v<bool, TCallback&, const DataBatch&>,
                  "batch callback must accept const DataBatch&, return bool and be noexcept");
    return [callback = std::move(callback)](ReadBlock& block) mutable noexcept -> bool {
        const auto batch = to_data_batch(block);
        return std::invoke(callback, batch);
    };
}

// All frames using one adapter must share DataLen, and records must not cross frame boundaries.
// Malformed data returns false and stays in the block; never skip it silently. Custom Block callbacks can query record_error().
template<class TDataLen = std::uint16_t, class TCallback>
auto make_record_callback(TCallback callback) {
    return [callback = std::move(callback)](ReadBlock& block) mutable noexcept -> bool {
        return read_records<TDataLen>(block, callback);
    };
}

// Like read_records, but prepend a fixed-width little-endian seq to each record (SeqWriter format).
// Record layout: [seq (sizeof(TSeq))] [length prefix (sizeof(DataLen))] [payload].
// Callback: void(std::int32_t thread_id, TSeq seq, const std::byte* data, std::size_t size) noexcept.
template<class TSeq, class TDataLen, class TCallback>
bool read_seq_records(ReadBlock& block, TCallback& callback) noexcept {
    static_assert(std::is_integral_v<TSeq> && std::is_unsigned_v<TSeq> &&
                  !std::is_same_v<TSeq, bool> &&
                  std::is_same_v<TSeq, std::remove_cv_t<TSeq>> && sizeof(TSeq) <= sizeof(std::uint64_t),
                  "TSeq must match SeqWriter's unsigned sequence type");
    static_assert(std::is_integral_v<TDataLen> && std::is_unsigned_v<TDataLen> &&
                  !std::is_same_v<TDataLen, bool> &&
                  std::is_same_v<TDataLen, std::remove_cv_t<TDataLen>> && sizeof(TDataLen) <= sizeof(std::uint64_t),
                  "DataLen must match SeqWriter's unsigned length type");
    static_assert(std::is_nothrow_invocable_r_v<void, TCallback&, std::int32_t, TSeq, const std::byte*, std::size_t>,
                  "seq record callback must accept (thread_id, seq, data, size) and be noexcept");
    if (block.record_error_ != ReadRecordError::none) return false;
    const auto batch = to_data_batch(block);
    while (block.record_frame_index_ < batch.frames.size()) {
        const auto& frame = batch.frames[block.record_frame_index_];
        while (block.record_offset_ < frame.size) {
            const auto remaining = frame.size - block.record_offset_;
            if (remaining < sizeof(TSeq) + sizeof(TDataLen)) {
                block.record_error_ = ReadRecordError::incomplete_length;
                return false;
            }
            const auto* const seq_ptr = frame.data + block.record_offset_;
            TSeq seq = 0;
            for (std::size_t i = 0; i < sizeof(TSeq); ++i)
                seq |= static_cast<TSeq>(std::to_integer<unsigned char>(seq_ptr[i])) << (i * 8);
            const auto* const prefix = seq_ptr + sizeof(TSeq);
            std::uint64_t length = 0;
            for (std::size_t i = 0; i < sizeof(TDataLen); ++i)
                length |= std::uint64_t{std::to_integer<unsigned char>(prefix[i])} << (i * 8);
            if (length > remaining - sizeof(TSeq) - sizeof(TDataLen)) {
                block.record_error_ = ReadRecordError::incomplete_payload;
                return false;
            }
            const auto size = static_cast<std::size_t>(length);
            std::invoke(callback, frame.thread_id, seq, prefix + sizeof(TDataLen), size);
            block.record_offset_ += sizeof(TSeq) + sizeof(TDataLen) + size;
        }
        ++block.record_frame_index_;
        block.record_offset_ = 0;
    }
    return true;
}

// Wrap a user callback as a sequence-record Block callback; TSeq / DataLen must match SeqWriter.
template<class TSeq, class TDataLen = std::uint16_t, class TCallback>
auto make_seq_record_callback(TCallback callback) {
    return [callback = std::move(callback)](ReadBlock& block) mutable noexcept -> bool {
        return read_seq_records<TSeq, TDataLen>(block, callback);
    };
}

// Like make_batch_callback, but invoke the object's fixed on_rcv_batch(const DataBatch&) member.
// The adapter borrows a reference without ownership; the object must outlive the entire HandoffQueue usage.
template<class TCallback>
auto make_batch_obj_callback(TCallback& callback) {
    static_assert(std::is_nothrow_invocable_r_v<bool, TCallback&, const DataBatch&>,
                  "callback must define on_rcv_batch(const DataBatch&) returning bool, noexcept");
    return [&callback](ReadBlock& block) noexcept -> bool {
        const auto batch = to_data_batch(block);
        return callback.on_rcv_batch(batch);
    };
}

// Like make_record_callback, but invoke the object's fixed on_rcv_record(thread_id, data, size) member.
template<class TDataLen = std::uint16_t, class TCallback>
auto make_record_obj_callback(TCallback& callback) {
    static_assert(std::is_nothrow_invocable_r_v<void, TCallback&, std::int32_t, const std::byte*, std::size_t>,
                  "callback must define on_rcv_record(thread_id, data, size), noexcept");
    auto record_callback = [&callback](std::int32_t thread_id, const std::byte* data, std::size_t size) noexcept -> void {
        callback.on_rcv_record(thread_id, data, size);
    };
    return [record_callback](ReadBlock& block) noexcept -> bool {
        return read_records<TDataLen>(block, record_callback);
    };
}


 
} // namespace TwoBufQueue
