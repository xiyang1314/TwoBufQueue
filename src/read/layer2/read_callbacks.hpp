#pragma once

#include "read_block.hpp"
#include <functional>
#include <type_traits>
#include <utility>

namespace TwoBufQueue {

// 与 Writer 的 DataLen 一致；前缀为小端、长度不含前缀，可包含空负载。每条数据开头会记录这一条的数据长度
// callback 每条被调用即确认一条，返回值被忽略；首参 thread_id 是写入来源编号。
// 进度属于 Block，取消领取后换一个线程也不重复已成功的记录。
template<class DataLen, class Callback>
bool read_records(ReadBlock& block, Callback& callback) noexcept {
    static_assert(std::is_integral_v<DataLen> && std::is_unsigned_v<DataLen> &&
                  !std::is_same_v<DataLen, bool> &&
                  std::is_same_v<DataLen, std::remove_cv_t<DataLen>> && sizeof(DataLen) <= sizeof(std::uint64_t),
                  "DataLen must match Writer's unsigned length type");
    static_assert(std::is_nothrow_invocable_r_v<void, Callback&, std::int32_t, const std::byte*, std::size_t>,
                  "record callback must accept (thread_id, data, size) and be noexcept");
    if (block.record_error_ != ReadRecordError::none) return false;
    const auto batch = to_data_batch(block);
    while (block.record_frame_index_ < batch.frames.size()) {
        const auto& frame = batch.frames[block.record_frame_index_];
        while (block.record_offset_ < frame.size) {
            const auto remaining = frame.size - block.record_offset_;
            if (remaining < sizeof(DataLen)) {
                block.record_error_ = ReadRecordError::incomplete_length;
                return false;
            }
            const auto* const prefix = frame.data + block.record_offset_;
            std::uint64_t length = 0;
            for (std::size_t i = 0; i < sizeof(DataLen); ++i)
                length |= std::uint64_t{std::to_integer<unsigned char>(prefix[i])} << (i * 8);
            if (length > remaining - sizeof(DataLen)) {
                block.record_error_ = ReadRecordError::incomplete_payload;
                return false;
            }
            const auto size = static_cast<std::size_t>(length);
            std::invoke(callback, frame.thread_id, prefix + sizeof(DataLen), size);
            block.record_offset_ += sizeof(DataLen) + size;
        }
        ++block.record_frame_index_;
        block.record_offset_ = 0;
    }
    return true;
}

// 整批 true 表示全部处理完；false 表示整批未处理，不能先产生部分不可重试的结果。
template<class Callback>
auto make_batch_callback(Callback callback) {
    static_assert(std::is_nothrow_invocable_r_v<bool, Callback&, const DataBatch&>,
                  "batch callback must accept const DataBatch&, return bool and be noexcept");
    return [callback = std::move(callback)](ReadBlock& block) mutable noexcept -> bool {
        const auto batch = to_data_batch(block);
        return std::invoke(callback, batch);
    };
}

// 使用同一适配器时所有 frame 必须使用相同 DataLen，且记录不能跨 frame。
// 格式错误返回 false 并留在块内，绝不静默跳过；自定义 Block 回调可查询 record_error()。
template<class DataLen = std::uint16_t, class Callback>
auto make_record_callback(Callback callback) {
    return [callback = std::move(callback)](ReadBlock& block) mutable noexcept -> bool {
        return read_records<DataLen>(block, callback);
    };
}

// 与 read_records 对应，但每条记录最前面多了固定大小的小端 seq（SeqWriter 布局）。
// 记录布局：[seq (sizeof(TSeq))] [长度前缀 (sizeof(DataLen))] [负载]
// 回调签名：void(std::int32_t thread_id, TSeq seq, const std::byte* data, std::size_t size) noexcept。
template<class TSeq, class DataLen, class Callback>
bool read_seq_records(ReadBlock& block, Callback& callback) noexcept {
    static_assert(std::is_integral_v<TSeq> && std::is_unsigned_v<TSeq> &&
                  !std::is_same_v<TSeq, bool> &&
                  std::is_same_v<TSeq, std::remove_cv_t<TSeq>> && sizeof(TSeq) <= sizeof(std::uint64_t),
                  "TSeq must match SeqWriter's unsigned sequence type");
    static_assert(std::is_integral_v<DataLen> && std::is_unsigned_v<DataLen> &&
                  !std::is_same_v<DataLen, bool> &&
                  std::is_same_v<DataLen, std::remove_cv_t<DataLen>> && sizeof(DataLen) <= sizeof(std::uint64_t),
                  "DataLen must match SeqWriter's unsigned length type");
    static_assert(std::is_nothrow_invocable_r_v<void, Callback&, std::int32_t, TSeq, const std::byte*, std::size_t>,
                  "seq record callback must accept (thread_id, seq, data, size) and be noexcept");
    if (block.record_error_ != ReadRecordError::none) return false;
    const auto batch = to_data_batch(block);
    while (block.record_frame_index_ < batch.frames.size()) {
        const auto& frame = batch.frames[block.record_frame_index_];
        while (block.record_offset_ < frame.size) {
            const auto remaining = frame.size - block.record_offset_;
            if (remaining < sizeof(TSeq) + sizeof(DataLen)) {
                block.record_error_ = ReadRecordError::incomplete_length;
                return false;
            }
            const auto* const seq_ptr = frame.data + block.record_offset_;
            TSeq seq = 0;
            for (std::size_t i = 0; i < sizeof(TSeq); ++i)
                seq |= static_cast<TSeq>(std::to_integer<unsigned char>(seq_ptr[i])) << (i * 8);
            const auto* const prefix = seq_ptr + sizeof(TSeq);
            std::uint64_t length = 0;
            for (std::size_t i = 0; i < sizeof(DataLen); ++i)
                length |= std::uint64_t{std::to_integer<unsigned char>(prefix[i])} << (i * 8);
            if (length > remaining - sizeof(TSeq) - sizeof(DataLen)) {
                block.record_error_ = ReadRecordError::incomplete_payload;
                return false;
            }
            const auto size = static_cast<std::size_t>(length);
            std::invoke(callback, frame.thread_id, seq, prefix + sizeof(DataLen), size);
            block.record_offset_ += sizeof(TSeq) + sizeof(DataLen) + size;
        }
        ++block.record_frame_index_;
        block.record_offset_ = 0;
    }
    return true;
}

// 把用户回调包装成 seq 记录的 Block 回调；TSeq / DataLen 必须与 SeqWriter 一致。
template<class TSeq, class DataLen = std::uint16_t, class Callback>
auto make_seq_record_callback(Callback callback) {
    return [callback = std::move(callback)](ReadBlock& block) mutable noexcept -> bool {
        return read_seq_records<TSeq, DataLen>(block, callback);
    };
}

// 与 make_batch_callback 等价，但回调传入对象并调用其固定成员 on_rcv_batch(const DataBatch&)。
// 返回的适配器仅持有引用、不转移所有权，对象生命周期需覆盖整个 HandoffQueue 使用期间。
template<class Callback>
auto make_batch_obj_callback(Callback& callback) {
    static_assert(std::is_nothrow_invocable_r_v<bool, Callback&, const DataBatch&>,
                  "callback must define on_rcv_batch(const DataBatch&) returning bool, noexcept");
    return [&callback](ReadBlock& block) noexcept -> bool {
        const auto batch = to_data_batch(block);
        return callback.on_rcv_batch(batch);
    };
}

// 与 make_record_callback 等价，但回调传入对象并调用其固定成员 on_rcv_record(thread_id, data, size)。
template<class DataLen = std::uint16_t, class Callback>
auto make_record_obj_callback(Callback& callback) {
    static_assert(std::is_nothrow_invocable_r_v<void, Callback&, std::int32_t, const std::byte*, std::size_t>,
                  "callback must define on_rcv_record(thread_id, data, size), noexcept");
    auto record_callback = [&callback](std::int32_t thread_id, const std::byte* data, std::size_t size) noexcept -> void {
        callback.on_rcv_record(thread_id, data, size);
    };
    return [record_callback](ReadBlock& block) noexcept -> bool {
        return read_records<DataLen>(block, record_callback);
    };
}


 
} // namespace TwoBufQueue
