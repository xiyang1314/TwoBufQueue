#include "../../support/joining_thread.hpp"
// SeqWriter prefixes each record with a fixed-width little-endian sequence number; consumers can use it to check ordering.
// Each producer owns a SeqWriter; shared atomic fetch_add assigns globally unique sequence numbers.
// Preserve order within each Writer; records from different Writers may interleave without a global ordering guarantee.

#include "../example_support.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <thread>
using namespace TwoBufQueue;

int main() try {
    //---------------1. Initialize 4 SeqWriters and TwoBufQueue---------------
    // SeqWriter<T> uses T for the sequence number; its callback takes one extra leading seq argument.
    using MyWriter = SeqWriter<std::uint32_t>;
    constexpr std::size_t writer_count = 4;
    constexpr std::uint32_t per_writer = 1000;
    std::atomic<std::uint32_t> next_seq{0};
    auto w0 = make_writer<MyWriter>(WriterSourceConfig{.thread_id = 0});
    auto w1 = make_writer<MyWriter>(WriterSourceConfig{.thread_id = 1});
    auto w2 = make_writer<MyWriter>(WriterSourceConfig{.thread_id = 2});
    auto w3 = make_writer<MyWriter>(WriterSourceConfig{.thread_id = 3});

    std::array<std::size_t, writer_count> received{};
    std::array<std::uint32_t, writer_count> last_seq{};
    std::array<bool, writer_count * per_writer> seen{};
    bool valid = true;
    auto rcvCallback = [&](std::int32_t thread_id, std::uint32_t seq, const std::byte* data, std::size_t size) noexcept -> void {
        if (thread_id < 0 || thread_id >= static_cast<std::int32_t>(writer_count) ||
            size != sizeof(std::uint32_t) || seq >= seen.size() || seen[seq]) {
            valid = false;
            return;
        }
        std::uint32_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        const auto id = static_cast<std::size_t>(thread_id);
        if (value != seq * 2 || (received[id] != 0 && seq <= last_seq[id])) valid = false;
        seen[seq] = true;
        last_seq[id] = seq;
        ++received[id];
    };

    ::TwoBufQueue::TwoBufQueue queue(rcvCallback, {&w0, &w1, &w2, &w3});

    //---------------2. Each producer writes through its Writer-------------
    std::array<bool, writer_count> written{};
    {
        std::array<OwnedWriter<MyWriter>*, writer_count> writers{&w0, &w1, &w2, &w3};
        std::array<brq_support::JoiningThread, writer_count> threads;
        for (std::size_t i = 0; i < writer_count; ++i)
            threads[i] = brq_support::JoiningThread([&, i] {
                written[i] = true;
                for (std::uint32_t n = 0; n < per_writer; ++n) {
                    const auto seq = next_seq.fetch_add(1, std::memory_order_relaxed);
                    if (!writers[i]->write(seq, static_cast<std::uint32_t>(seq * 2))) {
                        written[i] = false;
                        break;
                    }
                }
                writers[i]->finish(); // Each producer thread finishes its own Writer.
            });
    } // Join all producer threads before stopping the consumer.

    //---------------3. Finish----------------------------------------------
    queue.stop();      // Wait for every record to be consumed and stop the reader threads.

    //---------------4. Verify----------------------------------------------
    bool ok = valid && next_seq.load(std::memory_order_relaxed) == writer_count * per_writer;
    for (std::size_t i = 0; i < writer_count; ++i)
        ok = ok && written[i] && received[i] == per_writer;
    example::require(ok && queue.error_code() == ReadError::none, "seq delivery mismatch");
    std::puts("PASS: 4 writer threads, 4000 unique seq records, ordered per writer");
} catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
