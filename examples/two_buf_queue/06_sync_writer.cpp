// Synchronous callbacks run in the producer thread without a reader thread, which is useful for debugging.
// The rest matches asynchronous usage; specify the callback context when constructing the writer: make_writer<Writer<1, true, std::uint16_t, Context>>().
// With multiple writers/producer threads, the callback is automatically protected by a lock.
#include "../example_support.hpp"
#include <cstring>
using namespace TwoBufQueue;

int main() try {
    struct CustomData { int Data; };

    //---------------1. Initialization--------------------------------------
    std::size_t count = 0;
    const auto writer_thread = std::this_thread::get_id();
    bool same_thread = true;

    // Consumer callback.
    auto rcvCallback = [&](std::int32_t thread_id, const std::byte* data, std::size_t size) noexcept -> void {
        same_thread &= thread_id == 0 && std::this_thread::get_id() == writer_thread;
        if (++count <= 3) {
            std::printf("message: len = %.*s\n", static_cast<int>(size), reinterpret_cast<const char*>(data));
        } else {// No fixed payload type: decode according to the application's format.
            int type = 0;
            std::memcpy(&type, data, sizeof(type));
            std::printf("message: len = %d, type = %d\n", static_cast<int>(size), type);
        }
    };

    // A non-void Context selects synchronous handling. The callback type remains inlineable; TwoBufQueue selects locking from the Writer count.
    using Context = SyncContext<decltype(rcvCallback)>;
    auto writer = make_writer<Writer<1, true, std::uint16_t, Context>>(); // Only the Context template argument differs here.
    ::TwoBufQueue::TwoBufQueue queue(rcvCallback, writer); // Bind the callback during construction; do not start a reader thread.

    //---------------2. Producer writes-------------------------------------
    bool written = true;
    for (std::string_view text : {"hello", "TwoBufQueue", "goodbye"})
        written &= writer.write_mem(text.data(), static_cast<std::uint16_t>(text.size()));

    // No fixed payload type: the consumer callback decodes the received data using the application's format.
    written &= writer.write(1, 1, static_cast<short>(2));
    written &= writer.write(2, CustomData{3});

    //---------------3. Finish----------------------------------------------
    writer.finish(); // The application finishes the producer using the existing implementation.
    queue.stop();    // write has already handled each message; this returns the empty source block.

    //---------------4. Check the result------------------------------------
    example::require(written && count == 5 && same_thread, "write/drain or message count mismatch");
    std::puts("PASS: 5 messages, callback on writer thread");
} catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
