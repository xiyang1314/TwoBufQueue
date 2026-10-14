// TwoBufQueueBatch: one callback receives a batch rather than one record at a time.
// Useful when applications split a contiguous batch themselves, possibly processing its parts in parallel.

#include "../example_support.hpp"
using namespace TwoBufQueue;

int main() try {
    //---------------1. Initialize a frameless writer with no length prefix--
    auto writer = make_writer<Writer<1, false>>();

    std::size_t values = 0;
    // Consumer callback: receive a contiguous batch and split its records; see read_records for a parser.
    auto rcvCallback = [&](const DataBatch& batch) noexcept -> bool {
        for (const auto& frame : batch.frames)
            values += frame.size / sizeof(std::uint64_t); // Each record is one 8-byte value.
        return true; // The whole batch is handled; false rejects the entire batch for a later retry.
    }; // Batch storage may be released as soon as the callback returns. Parallel subtasks must all finish before returning; never retain the batch's pointers.

    ::TwoBufQueue::TwoBufQueueBatch queue(rcvCallback, writer);

    //---------------2. Produce 100 uint64_t records-------------------------
    bool written = true;
    for (std::uint64_t i = 0; i < 100; ++i)
        written &= writer.write(i);

    //---------------3. Finish----------------------------------------------
    writer.finish();
    queue.stop();

    //---------------4. Verify----------------------------------------------
    example::require(written && values == 100 &&
                     queue.error_code() == ReadError::none, "batch delivery mismatch");
    std::puts("PASS: 100 frameless records delivered in batches");
} catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
