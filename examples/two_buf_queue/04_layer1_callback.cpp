// Layer-1 callback: read the writer's source blocks directly, skipping layer-2 copying and per-record dispatch.
// Implement the six methods of ReadWriterDataCallback; the reader thread invokes them.

#include "../example_support.hpp"
#include <cstring>
using namespace TwoBufQueue;

// Consumer: implement the six layer-1 callbacks to process source-block bytes directly.
struct Collector {
    std::size_t count = 0;
    long long total = 0;

    // These six fixed methods form the ReadService interface; see read_writer_data_callback.hpp.
    void begin_read_complete(std::int64_t) noexcept {}     // Before a group of completed blocks.
    bool read_complete(SpscWriteBlock* block) noexcept {   // One full or finished block.
        const auto bytes = block->readable();              // The complete block's data range.
        return parse(block, bytes.data(), bytes.size());
    }
    void end_read_complete() noexcept {}
    void begin_read_current(std::int64_t) noexcept {}      // Before reading the current block still being written.
    bool read_current(SpscWriteBlock* block, const std::byte* ptr, std::size_t size) noexcept {
        return parse(block, ptr, size);                    // Newly available data in the current block.
    }
    void end_read() noexcept {}

private:
    // Parse [ptr, ptr + size) as [length prefix][payload] records, using the same format as example_support.
    bool parse(SpscWriteBlock* block, const std::byte* ptr, std::size_t size) noexcept {
        return example::messages(block, ptr, size,
            [&](std::int32_t, const std::byte* data, std::size_t n) noexcept {
                if (n != sizeof(int)) return false;
                int value = 0;
                std::memcpy(&value, data, sizeof(value));
                total += value;
                ++count;
                return true;
            });
    }
};

int main() try {
    //---------------1. Initialize one writer and one layer-1 reader---------
    auto writer = make_writer<Writer<>>();                 // Default Writer includes length prefixes.
    Collector collector;
    ReadWorker reader({}, collector);                      // {} selects the default ReadConfig.
    if (!reader.add_writer(&writer)) throw std::runtime_error("add_writer failed");
    if (!reader.start()) throw std::runtime_error("start failed");

    //---------------2. Produce 100 int records------------------------------
    bool written = true;
    for (int i = 1; i <= 100; ++i)
        written &= writer.write(i);

    //---------------3. Finish the producer, then drain and stop the reader--
    writer.finish();                                       // End production and hand off the final block.
    reader.stop();                                         // Wait for the remaining data before stopping.

    //---------------4. Verify----------------------------------------------
    example::require(written && reader.error_code() == ReadError::none, "write/read failed");
    example::require(collector.count == 100 && collector.total == 5050, "delivery mismatch");
    std::puts("PASS: 100 records read directly at layer 1 (no layer-2 copy)");
} catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
