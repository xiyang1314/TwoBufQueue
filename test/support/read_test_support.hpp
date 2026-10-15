#pragma once
#include "writer_fixture.hpp"
#include "TwoBufQueue/src/read/read_layer1/read_worker.hpp"
#include "TwoBufQueue/src/write/writer.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <source_location>
#include <string_view>
#include <thread>
#include <vector>

namespace read_test {
using namespace TwoBufQueue;
using namespace std::chrono_literals;
inline void check(bool ok, const char* expression,
                  std::source_location at = std::source_location::current()) noexcept {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s:%u: %s\n", at.file_name(), at.line(), expression);
        std::abort();
    }
}
#define CHECK(...) ::read_test::check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__)
template<class TPredicate> bool wait_until(TPredicate predicate, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}
inline ReadConfig config() {
    ReadConfig result;
    result.idle_spin_count = 0;
    result.idle_rescan_interval = 1ms;
    result.idle_rescan_max_interval = 8ms;
    result.finish_timeout = 2s;
    return result;
}
inline unsigned value_at(const std::byte* ptr) noexcept {
    unsigned value;
    std::memcpy(&value, ptr + 2, sizeof(value));
    return value;
}
struct Event {
    char kind;
    int source = -1;
    std::size_t size = 0;
    bool operator==(const Event&) const = default;
};
struct Record { int source; unsigned value; char phase; std::int64_t time; };

// Ordinary callback type, without inheritance or private access to production classes.
// Read vectors/hooks only after the worker joins, or while it waits at an explicit gate.
struct Probe {
    std::vector<Event> events;
    std::vector<Record> records;
    std::atomic<bool> accept{true};
    std::atomic<unsigned> completed_calls{0}, current_calls{0}, accepted{0};
    std::function<void()> begin_complete_hook, begin_current_hook, end_hook;
    std::function<bool(char, SpscWriteBlock*, const std::byte*, std::size_t)> decide;
    std::int64_t timestamp = 0;
    Probe() { events.reserve(200'000); records.reserve(200'000); }
    void begin_read_complete(std::int64_t time) noexcept {
        timestamp = time; events.push_back({'B'});
        if (begin_complete_hook) begin_complete_hook();
    }
    bool read_complete(SpscWriteBlock* block) noexcept {
        completed_calls.fetch_add(1);
        const auto bytes = block->readable();
        return read('F', block, bytes.data(), bytes.size());
    }
    void end_read_complete() noexcept { events.push_back({'E'}); }
    void begin_read_current(std::int64_t time) noexcept {
        timestamp = time; events.push_back({'b'});
        if (begin_current_hook) begin_current_hook();
    }
    bool read_current(SpscWriteBlock* block, const std::byte* ptr, std::size_t size) noexcept {
        current_calls.fetch_add(1);
        return read('C', block, ptr, size);
    }
    void end_read() noexcept { events.push_back({'e'}); if (end_hook) end_hook(); }
    bool read(char phase, SpscWriteBlock* block, const std::byte* ptr, std::size_t size) noexcept {
        events.push_back({phase, block->source_id(), size});
        if (decide && !decide(phase, block, ptr, size)) return false;
        if (!accept.load()) return false;
        CHECK(size != 0 && size % 6 == 0 && timestamp > 0);
        for (std::size_t offset = 0; offset != size; offset += 6) {
            CHECK(ptr[offset] == std::byte{4} && ptr[offset + 1] == std::byte{0});
            records.push_back({block->source_id(), value_at(ptr + offset), phase, timestamp});
        }
        accepted.fetch_add(static_cast<unsigned>(size / 6));
        return true;
    }
    void expect(std::initializer_list<Event> expected) const {
        CHECK(events == std::vector<Event>(expected));
    }
    std::vector<unsigned> values(int source) const {
        std::vector<unsigned> result;
        for (const auto& record : records) if (record.source == source) result.push_back(record.value);
        return result;
    }
    void verify(int source, unsigned count) const {
        const auto got = values(source);
        CHECK(got.size() == count);
        for (unsigned i = 0; i < count; ++i) CHECK(got[i] == i);
    }
};
struct Rig {
    WriterFixture source;
    Writer<> writer;
    Probe probe;
    AutoResetEvent event;
    ReadService<Probe> service;
    std::atomic<bool> cancel{false};
    explicit Rig(std::size_t bytes = 24, std::size_t blocks = 8, ReadConfig cfg = config())
        : source(bytes, blocks, blocks), writer(&source.queue, 0, source.backpressure),
          service(cfg, probe, event) { CHECK(service.add_writer(&writer)); }
    ReadResult tick(bool force = true) { return service.read_once(force, cancel); }
    void drain() {
        probe.accept = true; probe.decide = {}; probe.begin_complete_hook = {};
        probe.begin_current_hook = {}; cancel = false; writer.finish();
        const auto result = tick();
        CHECK(result.error == ReadError::none && !result.pending && source.all_released());
    }
};
struct Gate {
    std::atomic<bool> entered{false}, resume{false};
    void pause() noexcept {
        entered.store(true, std::memory_order_release);
        CHECK(wait_until([&] { return resume.load(std::memory_order_acquire); }));
    }
    void wait() { CHECK(wait_until([&] { return entered.load(std::memory_order_acquire); })); }
    void release() { resume.store(true, std::memory_order_release); }
};
struct Case { std::string_view name; void (*run)(); };
inline int run_cases(int argc, char** argv, std::initializer_list<Case> cases) {
    if (argc > 2) return 2;
    bool matched = false;
    for (const auto& test : cases) {
        if (argc == 2 && argv[1] != test.name) continue;
        test.run(); matched = true;
        std::printf("PASS %.*s\n", static_cast<int>(test.name.size()), test.name.data());
    }
    return matched ? 0 : 2;
}
} // namespace read_test
