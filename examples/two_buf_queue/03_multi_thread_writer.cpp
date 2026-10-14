#include "../../support/joining_thread.hpp"
// Multiple producers: one thread owns each writer; one TwoBufQueue gathers all writers.
// Only one thread may call write on a writer (checked internally); thread_id identifies the source.
// read_layer1_thread_count enables multiple layer-1 readers, allowing concurrency at both ends.

#include "../example_support.hpp"
#include <array>
#include <cstring>
#include <thread>
using namespace TwoBufQueue;

int main() try {
    //---------------1. Initialize 4 writers (unique thread_id) and 2 readers-
    // read_layer1_thread_count = 2: split the four writers evenly; each layer-1 reader copies two writers into layer 2 concurrently.
    constexpr std::size_t writer_count = 4, per_writer = 1000, read_layer1_thread_count = 2;
    auto w0 = make_writer<Writer<>>(WriterSourceConfig{.thread_id = 0});
    auto w1 = make_writer<Writer<>>(WriterSourceConfig{.thread_id = 1});
    auto w2 = make_writer<Writer<>>(WriterSourceConfig{.thread_id = 2});
    auto w3 = make_writer<Writer<>>(WriterSourceConfig{.thread_id = 3});

    // Route callbacks by thread_id and verify each source's ordering and count.
    std::array<std::uint64_t, writer_count> next{};
    std::array<std::size_t, writer_count> received{};
    bool valid = true;
    auto rcvCallback = [&](std::int32_t thread_id, const std::byte* data, std::size_t size) noexcept -> void {
        if (thread_id < 0 || static_cast<std::size_t>(thread_id) >= writer_count ||
            size != sizeof(std::uint64_t)) { valid = false; return; }
        std::uint64_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        const auto id = static_cast<std::size_t>(thread_id);
        if (value != next[id]++) valid = false;
        ++received[id];
    };

    ::TwoBufQueue::TwoBufQueue queue(
        rcvCallback,
        {&w0, &w1, &w2, &w3},                                        // Multiple writers.
        TwoBufQueueOptions{.read_layer1_thread_count = read_layer1_thread_count}); // Multiple readers.

    //---------------2. Each writer thread sends per_writer records----------
    std::array<bool, writer_count> written{};
    {
        std::array<OwnedWriter<Writer<>>*, writer_count> writers{&w0, &w1, &w2, &w3};
        std::array<brq_support::JoiningThread, writer_count> threads;
        for (std::size_t i = 0; i < writer_count; ++i)
            threads[i] = brq_support::JoiningThread([&, i] {
                written[i] = true;
                for (std::size_t j = 0; j < per_writer; ++j)
                    if (!writers[i]->write(static_cast<std::uint64_t>(j))) { written[i] = false; break; }
                writers[i]->finish();
            });
    } // Join every writer thread when leaving this scope.

    //---------------3. Finish----------------------------------------------
    queue.stop();  // Consume all remaining records and stop every reader thread.

    //---------------4. Verify each writer's ordering and record count-------
    bool ok = valid;
    for (std::size_t i = 0; i < writer_count; ++i)
        ok = ok && written[i] && received[i] == per_writer;
    example::require(ok && queue.error_code() == ReadError::none, "multi-thread writer mismatch");
    std::puts("PASS: 4 writer threads, 2 read threads, 4000 records");
} catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
