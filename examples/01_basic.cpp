#include "example_support.hpp"
#include <cstring>
using namespace TwoBufQueue;

int main() try {
    struct CustomData { int Data; };

    //---------------1. Initialization--------------------------------------
    std::size_t count = 0; 
    auto writer = make_writer<Writer<>>();// The producer's writer object.

    // Consumer callback.
    auto rcvCallback = [&](std::int32_t thread_id, const std::byte* data, std::size_t size) noexcept -> void {
        (void)thread_id; // One writer; thread_id is always 0.
        if (++count <= 3) {
            std::printf("message: len = %.*s\n", static_cast<int>(size), reinterpret_cast<const char*>(data));
        } else {// No fixed payload type: decode according to the application's format.
            int type = 0;
            std::memcpy(&type, data, sizeof(type));
            std::printf("message: len = %d, type = %d\n", static_cast<int>(size), type);
        }
    };

    ::TwoBufQueue::TwoBufQueue queue(rcvCallback, writer); // Queue that transfers the data.
    
    //---------------2. Producer writes-------------------------------------
    bool written = true;
    for (std::string_view text : {"hello", "TwoBufQueue", "goodbye"})
        written &= writer.write_mem(text.data(), static_cast<std::uint16_t>(text.size()));
 
    // No fixed payload type: the consumer callback decodes the received data using the application's format.
    written &= writer.write(1, 1, static_cast<short>(2));     
    written &= writer.write(2, CustomData{3});        

    //---------------3. Finish----------------------------------------------
    writer.finish(); // The application is responsible for finishing the producer.
    queue.stop();    // Wait until all records are consumed, then stop the threads.

    //---------------4. Check the result------------------------------------
    example::require(written && count == 5, "write/drain or message count mismatch");
    std::puts("PASS: 5 messages");
} catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
