#include "../example_support.hpp"
#include <array>
#include <chrono>
#include <cstring>
using namespace TwoBufQueue;

int main() try {
    //---------------1. Initialization--------------------------------------
    using HotWriter = Writer<UINT32_MAX>; // Publish at block handoff and keep length prefixes; Session needs no additional template argument.
    auto writer = make_writer<HotWriter>({.block_size = 4096, .pool_min = 8, .pool_max = 8});
    std::size_t count = 0;
    auto rcvCallback = [&](std::int32_t thread_id, const std::byte* data, std::size_t size) noexcept {
        (void)thread_id;
        std::array<std::uint64_t, 4> value{};
        if (size != sizeof(value)) std::terminate();
        std::memcpy(value.data(), data, size);
        if (value != std::array<std::uint64_t,4>{count,count+1,count+2,count+3}) std::terminate();
        ++count;
    };
    ::TwoBufQueue::TwoBufQueue queue(rcvCallback, writer);

    //---------------2. Producer writes-------------------------------------
    std::uint64_t sent = 0;
    auto session = writer.get_current_session(std::chrono::seconds{1}); // Handle acquisition failure and return.
    example::require(static_cast<bool>(session), "cannot acquire session within timeout");
    while (sent != 10000) { // Outer loop: the caller explicitly selects the next block.
        while (sent != 10000) { // Inner loop: write only into this block; leave the loop when it lacks space.
            const auto result = session.write(std::array<std::uint64_t,4>{sent,sent+1,sent+2,sent+3});
            if (result == SessionWriteResult::insufficient_space) break;
            example::require(result == SessionWriteResult::success, "record exceeds block capacity or write error");
            ++sent; // Advance the message position only after success.
        }
        if (sent == 10000) break; // All messages are written; do not acquire another block.
        session = {}; // End the old Session: synchronize the cursor only; neither publish nor switch blocks.
        session = writer.get_next_session(std::chrono::seconds{1});
        example::require(static_cast<bool>(session), "cannot acquire session within timeout");
    }
    session.flush(); // Optional explicit flush: publish the position without switching blocks or ending the Session.
    session = {}; // End the Session before calling Writer.finish; scope-based destruction works too.

    //---------------3. Finish----------------------------------------------
    // With current scanning disabled, flush does not hand off the tail; finish hands off the final block.
    writer.finish();
    queue.stop();
    //---------------4. Check the result------------------------------------
    example::require(count == 10000, "message count mismatch");
    std::puts("PASS: 10000 messages with current/next single-block sessions");
} catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
