#pragma once

#include "TwoBufQueue/src/data/data_batch.hpp"
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace TwoBufQueue {

enum class ReadRecordError { none, incomplete_length, incomplete_payload };

// Nonintrusive resource owning data/frame arrays; WarmPool requires only reset(generation).
// A producer exclusively appends; after send, a claimant exclusively reads and tracks record progress. No block-local lock needed.
class ReadBlock {
    template<class DataLen, class Callback>
    friend bool read_records(ReadBlock&, Callback&) noexcept;
    template<class TSeq, class DataLen, class Callback>
    friend bool read_seq_records(ReadBlock&, Callback&) noexcept;
public:
    static constexpr std::size_t block_alignment = 64;

    ReadBlock(std::size_t capacity, std::size_t frame_capacity)
        : capacity_(checked_capacity(capacity)),
          frame_capacity_(checked_frame_capacity(frame_capacity)) {
        data_ = static_cast<std::byte*>(::operator new(capacity_, std::align_val_t{block_alignment}));
        try {
            frames_ = new DataFrame[frame_capacity_];
        } catch (...) {
            ::operator delete(data_, std::align_val_t{block_alignment});
            throw;
        }
    }
    ~ReadBlock() noexcept {
        delete[] frames_;
        ::operator delete(data_, std::align_val_t{block_alignment});
    }
    ReadBlock(const ReadBlock&) = delete;
    ReadBlock& operator=(const ReadBlock&) = delete;

    bool reset(std::uint64_t /*generation*/) noexcept {
        data_size_ = frame_count_ = record_frame_index_ = record_offset_ = 0;
        record_error_ = ReadRecordError::none;
        return true;
    }

    // Copy the full range; failure leaves data, frame count and consumption position unchanged.
    // data must not overlap this block's storage. Empty ranges do not register frames.
    [[nodiscard]] bool append(std::int32_t writer_id, const std::byte* data,
                              std::size_t size, std::int64_t read_time_ns) noexcept {
        if (writer_id < 0 || (size && !data)) return false;
        if (!size) return true;
        if (size > capacity_ - data_size_ || frame_count_ == frame_capacity_) return false;
        auto* const target = data_ + data_size_;
        std::memcpy(target, data, size);
        frames_[frame_count_++] = {writer_id, target, size, read_time_ns};
        data_size_ += size;
        return true;
    }

    [[nodiscard]] std::span<const DataFrame> frames() const noexcept { return {frames_, frame_count_}; }
    [[nodiscard]] std::size_t data_size() const noexcept { return data_size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t frame_capacity() const noexcept { return frame_capacity_; }
    // Query only by the owning consumer; distinguish malformed records from ordinary callback rejection.
    [[nodiscard]] ReadRecordError record_error() const noexcept { return record_error_; }

    static std::size_t checked_capacity(std::size_t capacity) {
        if (!capacity || capacity > static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()))
            throw std::invalid_argument("ReadBlock capacity must be positive and fit ptrdiff_t");
        return capacity;
    }
    static std::size_t checked_frame_capacity(std::size_t capacity) {
        if (!capacity || capacity > (std::numeric_limits<std::size_t>::max)() / sizeof(DataFrame))
            throw std::invalid_argument("ReadBlock frame capacity must be positive and fit an array");
        return capacity;
    }

private:
    const std::size_t capacity_;
    const std::size_t frame_capacity_;
    std::byte* data_ = nullptr;
    DataFrame* frames_ = nullptr;
    std::size_t data_size_ = 0;
    std::size_t frame_count_ = 0;
    std::size_t record_frame_index_ = 0;
    std::size_t record_offset_ = 0;
    ReadRecordError record_error_ = ReadRecordError::none;
};

[[nodiscard]] inline DataBatch to_data_batch(const ReadBlock& block) noexcept {
    return {block.frames(), block.data_size()};
}

} // namespace TwoBufQueue
