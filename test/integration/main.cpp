// Copyright 2026 chuan lu
// SPDX-License-Identifier: Apache-2.0
#include <TwoBufQueue.hpp>
#include <TwoBufQueue/src/write/writer.hpp>
#include <array>
#include <cstring>

int main() {
    auto writer = TwoBufQueue::make_writer<TwoBufQueue::Writer<>>();
    std::size_t count = 0;
    bool valid = true;
    auto callback = [&](std::int32_t id, const std::byte* data, std::size_t size) noexcept {
        std::array<std::uint64_t, 4> values{};
        if (id != 0 || size != sizeof(values)) { valid = false; return; }
        std::memcpy(values.data(), data, size);
        valid &= values == std::array<std::uint64_t,4>{count,count+1,count+2,count+3};
        ++count;
    };
    TwoBufQueue::TwoBufQueue queue(callback, writer);
    bool wrote = true;
    for (std::uint64_t i = 0; i != 1000; ++i)
        wrote &= writer.write(std::array<std::uint64_t,4>{i,i+1,i+2,i+3});
    writer.flush();
    writer.finish();
    queue.stop();
    return wrote && valid && count == 1000 &&
        queue.error_code() == TwoBufQueue::ReadError::none ? 0 : 1;
}
