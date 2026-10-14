// Copyright 2026 chuan lu
// SPDX-License-Identifier: Apache-2.0
#include "../example_support.hpp"
#include <array>
#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
using namespace TwoBufQueue;

int main(int argc, char** argv) try {
    // Usage: brq_tbq_09_async_logger [output-file] [--slow-sink] [--drop]
    const char* path = "twobufqueue-example.log";
    bool slow_sink = false, drop = false, path_set = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--slow-sink") slow_sink = true;
        else if (std::string_view{argv[i]} == "--drop") drop = true;
        else if (!path_set && !std::string_view{argv[i]}.starts_with("--")) {
            path = argv[i]; path_set = true;
        } else throw std::invalid_argument("expected [output-file] [--slow-sink] [--drop]");
    }
    struct LogRecord {
        std::uint64_t sequence;
        std::uint32_t request_id;
        std::array<char, 80> text;
    };

    //---------------1. Initialization--------------------------------------
    // File lifetime covers the callback threads. Only the callback writes it.
    using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
    File file(std::fopen(path, "wb"), &std::fclose);
    example::require(static_cast<bool>(file), "cannot open log file");
    WriterSourceConfig source;
    source.block_size = 4096;
    source.pool_min = source.pool_max = 2; // Deliberately small, to expose pressure.
    source.backpressure.mode = drop ? BackpressureMode::Drop : BackpressureMode::Block;
    source.backpressure.wait_get_timeout = std::chrono::seconds{2};
    auto writer = make_writer<Writer<32>>(source);
    std::size_t written = 0;
    bool sink_error = false;
    std::thread::id callback_thread;
    const auto producer_thread = std::this_thread::get_id();
    auto rcvCallback = [&](std::int32_t thread_id, const std::byte* data,
                           std::size_t size) noexcept -> void {
        if (written == 0) callback_thread = std::this_thread::get_id();
        // Simulate a temporarily slow file system; not enabled in normal runs.
        if (slow_sink && written == 0) std::this_thread::sleep_for(std::chrono::milliseconds{20});
        LogRecord record{};
        if (thread_id != 0 || size != sizeof(record)) { sink_error = true; return; }
        std::memcpy(&record, data, sizeof(record)); // Callback data can be unaligned.
        if (!std::memchr(record.text.data(), '\0', record.text.size())) {
            sink_error = true; return;
        }
        if (std::fprintf(file.get(), "%llu request=%u %s\n",
                static_cast<unsigned long long>(record.sequence),
                static_cast<unsigned>(record.request_id), record.text.data()) < 0)
            sink_error = true; // noexcept callback records I/O errors; no implicit retry.
        ++written;
    };
    TwoBufQueueOptions options;
    options.output_block_size = 4096; // Can hold a complete source block.
    options.output_pool_min = options.output_pool_max = 2;
    options.output_queue_capacity = 2;
    options.read_config.is_scan_current = true; // flush can expose a partial block.
    ::TwoBufQueue::TwoBufQueue queue(rcvCallback, writer, options);

    //---------------2. Producer writes-------------------------------------
    // Business work and record construction stay on the application thread.
    // File I/O runs on the independent callback thread, after layer-2 copying.
    std::vector<std::uint64_t> accepted;
    accepted.reserve(10000);
    std::size_t failed = 0;
    for (std::uint64_t sequence = 0; sequence != 10000; ++sequence) {
        LogRecord record{};
        record.sequence = sequence;
        record.request_id = static_cast<std::uint32_t>(sequence % 100);
        std::snprintf(record.text.data(), record.text.size(), "request completed");
        if (writer.write(record)) accepted.push_back(sequence);
        else ++failed; // Drop rejects this record; Block can also time out.
        if ((sequence + 1) % 1000 == 0) writer.flush();
    }
    writer.flush(); // Publish + notify; does not wait for callbacks or flush the file.

    //---------------3. Finish----------------------------------------------
    writer.finish(); // Stop producing and hand off the final source block.
    queue.stop();    // Drain copying and callbacks before closing the file.
    const auto pressure = writer.backpressure_stats();
    const bool flush_ok = std::fflush(file.get()) == 0;
    const bool close_ok = std::fclose(file.release()) == 0;

    //---------------4. Check the result------------------------------------
    example::require(queue.error_code() == ReadError::none, "queue did not drain cleanly");
    example::require(!sink_error && flush_ok && close_ok, "file I/O failed");
    example::require(written == accepted.size() && callback_thread != producer_thread,
                     "callback count or thread mismatch");
    std::ifstream saved(path, std::ios::binary);
    example::require(saved.is_open(), "cannot reopen saved log");
    std::string line;
    for (const auto sequence : accepted) {
        const auto expected = std::to_string(sequence) + " request=" +
            std::to_string(sequence % 100) + " request completed";
        example::require(static_cast<bool>(std::getline(saved, line)) && line == expected,
                         "saved record is missing, reordered or corrupted");
    }
    example::require(!std::getline(saved, line) && saved.eof(), "unexpected record or read error");
    std::printf("PASS: %zu saved, %zu rejected, pressure=%llu, blocked=%llu; file=%s\n",
        written, failed, static_cast<unsigned long long>(pressure.pressure_events),
        static_cast<unsigned long long>(pressure.blocked_events), path);
    example::require(drop || failed == 0, "blocking writer timed out; rejected records were reported");
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what()); return 1;
}
