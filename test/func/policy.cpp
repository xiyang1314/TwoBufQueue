#include "../../support/joining_thread.hpp"
#include "../writer_fixture.hpp"
#include "../warm_pool_test_support.hpp"
#include "../block_test_support.hpp"
#include "TwoBufQueue/src/utils/no_lock.hpp"
#include "TwoBufQueue/src/utils/spin_lock.hpp"
#include <type_traits>
#include "support.hpp"
namespace TwoBufQueue {
class BlockSlicesAllocatorForTest {
  public:
    template <class B> static void release(B *b, std::size_t index, std::uint64_t generation) {
        b->slices_.release(index, generation, std::source_location::current());
    }
};
} // namespace TwoBufQueue
namespace ft {
template <class B> void tolerant(std::string kind) {
    auto pool = make_block_pool<TestBlockPool<B, false>>(64, 1, 1,
                                                        {.is_check_slice_release = false});
    auto *b = pool.get().get();
    auto first = b->allocate(8);
    auto second = b->allocate(8);
    if (kind == "old") {
        first.release();
        second.release();
        finish_block(b);
        b = pool.get().get();
        first = b->allocate(8);
        second = b->allocate(8);
    }
    if (kind == "free") {
        first.release();
        second.release();
        finish_block(b);
    }
    if (kind == "duplicate")
        first.release();
    if (kind == "free") CHECK(pool.all_released());
    const auto outstanding = b->outstanding_count(), inuse = pool.in_use_count();
    BlockSlicesAllocatorForTest::release(b, kind == "index" ? 999 : 0, kind == "zero" ? 0 : 1);
    CHECK(b->outstanding_count() == outstanding);
    CHECK(pool.in_use_count() == inuse);
    if (kind != "free") {
        finish_block(b);
        if (first)
            first.release();
        if (second)
            second.release();
    }
    CHECK(pool.in_use_count() == 0);
    b = pool.get().get();
    auto again = b->allocate(8);
    CHECK(again);
    finish_block(b);
    again.release();
    CHECK(pool.in_use_count() == 0);
}
template <class B> void complete_lifecycle() {
    auto pool = make_block_pool<TestBlockPool<B, false>>(64, 1, 1);
    std::atomic<unsigned> recycled{0};
    pool.set_release_callback([](void *context) noexcept {
        static_cast<std::atomic<unsigned> *>(context)->fetch_add(1);
    }, &recycled);
    for (unsigned round = 0; round < 3; ++round) {
        auto *b = pool.get().get();
        CHECK(b && b->state() == ReaderBlockState::Allocating);
        auto first = b->allocate(8);
        CHECK(first);
        first.release();
        CHECK(b->outstanding_count() == 0);
        CHECK(b->state() == ReaderBlockState::Allocating);
        CHECK(pool.in_use_count() == 1);
        CHECK(recycled.load() == round);
        auto held = b->allocate(8);
        CHECK(held);
        std::memset(held.data(), 0x5a, held.size());
        b->to_alloc_finished();
        // held pins this generation, so inspecting the block is safe in this test.
        CHECK(b->state() == ReaderBlockState::AllocFinished);
        CHECK(b->outstanding_count() == 1);
        CHECK(pool.in_use_count() == 1);
        CHECK(recycled.load() == round);
        for (std::size_t i = 0; i < held.size(); ++i)
            CHECK(held.data()[i] == std::byte{0x5a});
        held.release();
        CHECK(pool.in_use_count() == 0);
        CHECK(recycled.load() == round + 1);
    }
    auto *empty = pool.get().get();
    CHECK(empty);
    finish_block(empty);
    CHECK(pool.in_use_count() == 0);
    CHECK(recycled.load() == 4);
    auto *all_released = pool.get().get();
    CHECK(all_released);
    auto transient = all_released->allocate(8);
    CHECK(transient);
    transient.release();
    CHECK(pool.in_use_count() == 1);
    finish_block(all_released);
    CHECK(pool.in_use_count() == 0);
    CHECK(recycled.load() == 5);
    pool.set_release_callback(nullptr, nullptr);
}
static void source_completion_phases() {
    Direct<> d(64, 1);
    CHECK(d.writer.write(42u));
    auto* range = d.writer.source_block();
    d.writer.finish();
    CHECK(range->state() == SpscWriteBlockState::WriteFinished && d.pool.in_use_count() == 1);
    d.receiver.reject = true;
    tick(d.service);
    CHECK(range->read_pos() == 0 && d.pool.in_use_count() == 1);
    d.receiver.reject = false;
    d.drain();
    CHECK(d.receiver.good && d.receiver.messages.size() == 1);
    CHECK(d.pool.handoff_queue().empty() && d.pool.all_released());
}
static const char *reason = nullptr;
void policy_cases(Cases &c) {
    c.push_back({"M_source_complete", source_completion_phases});
    c.push_back({"M_complete_11", complete_lifecycle<Alloc1Release1ReaderBlock>});
    c.push_back({"M_complete_1N", complete_lifecycle<Alloc1ReleaseNReaderBlock>});
    c.push_back({"M_complete_NN", complete_lifecycle<AllocNReleaseNReaderBlock>});
    for (std::string mode : {"11", "1N", "NN"})
        for (std::string k : {"old", "zero", "free", "index", "duplicate"})
            c.push_back({"P_tolerant_" + mode + "_" + k, [mode, k] {
                             if (mode == "11")
                                 tolerant<Alloc1Release1ReaderBlock>(k);
                             else if (mode == "1N")
                                 tolerant<ReaderBlock<>>(k);
                             else
                                 tolerant<AllocNReleaseNReaderBlock>(k);
                         }});
    for (std::string k : {"old", "free", "index", "duplicate"})
        c.push_back({"D_strict_" + k, [k] {
                         using P = TestBlockPool<ReaderBlock<>>;
                         TestDiagnostics<P>::set_error_handler([](const TestDiagnostics<P>::ErrorInfo &e) noexcept {
                             if (reason && std::strcmp(reason, e.reason) == 0) {
                                 std::fputs("EXPECTED_CONTRACT_FAILURE\n", stderr);
                                 std::fflush(stderr);
                                 std::_Exit(86);
                             }
                             std::fprintf(stderr, "WRONG_REASON: %s\n", e.reason);
                             std::_Exit(87);
                         });
                         auto p = make_block_pool<P>(64, 1, 1);
                         auto *b = p.get().get();
                         auto a = b->allocate(1);
                         auto other = b->allocate(1);
                         if (k == "old") {
                             reason = "slice belongs to an old block generation";
                             a.release();
                             other.release();
                             finish_block(b);
                             b = p.get().get();
                             a = b->allocate(1);
                         }
                         if (k == "free") {
                             reason = "block is already released";
                             a.release();
                             other.release();
                             finish_block(b);
                         }
                         if (k == "free") CHECK(p.all_released());
                         if (k == "index")
                             reason = "slice index is out of range";
                         if (k == "duplicate") {
                             reason = "slice is already released";
                             a.release();
                         }
                         BlockSlicesAllocatorForTest::release(b, k == "index" ? 999 : 0, 1);
                         throw std::runtime_error("missing strict release diagnostic");
                     }});
    c.push_back({"W_pressure_timeout_invalid", [] {
                     const auto limit = std::chrono::duration_cast<std::chrono::milliseconds>(
                         AutoResetEvent::Clock::duration::max());
                     for (const auto timeout : {0ms, -1ms, limit + 1ms, std::chrono::milliseconds::max()})
                         invalid([&] { WriterFixture pool(64, 0, 1); Writer<> writer(&pool.queue, 0, {.wait_get_timeout = timeout}); });
                 }});
    c.push_back({"W_pressure_timeout_limit", [] {
                     const auto limit = std::chrono::duration_cast<std::chrono::milliseconds>(
                         AutoResetEvent::Clock::duration::max());
                     WriterFixture pool(64, 0, 1); Writer<> writer(&pool.queue, 0, {.wait_get_timeout = limit});
                     CHECK(writer.source_block() && pool.block_count() == 1 && !pool.all_released());
                 }});
    c.push_back({"W_pressure_spin_timeout", [] {
                     Direct<> d(6, 1, {.mode = BackpressureMode::Block,
                                      .spin_count = (std::numeric_limits<std::uint32_t>::max)(),
                                      .wait_get_timeout = 30ms});
                     d.service.prepare_start();
                     CHECK(d.writer.write(1u));
                     std::uint64_t previous_ticks = 0, longest = 0;
                     for (unsigned attempt = 1; attempt <= 2; ++attempt) {
                         const auto begin = std::chrono::steady_clock::now();
                         CHECK(!d.writer.write(2u));
                         const auto elapsed = std::chrono::steady_clock::now() - begin;
                         CHECK(elapsed >= 30ms && elapsed < 2s);
                         const auto stats = d.writer.backpressure_stats();
                         CHECK(stats.blocked_events == attempt && stats.dropped_records == attempt);
                         CHECK(stats.blocked_ticks > previous_ticks);
                         longest = std::max(longest, stats.blocked_ticks - previous_ticks);
                         CHECK(stats.max_blocked_ticks == longest);
                         previous_ticks = stats.blocked_ticks;
                     }
                     tick(d.service);
                     CHECK(d.writer.write(2u));
                     d.drain();
                     CHECK(d.receiver.messages.size() == 2);
                     for (unsigned i = 0; i < 2; ++i) {
                         const auto expected = framed(i + 1);
                         CHECK(d.receiver.messages[i].bytes ==
                               std::vector<std::byte>(expected.begin() + 2, expected.end()));
                     }
                     CHECK(d.pool.all_released());
                 }});
    c.push_back({"W_pressure_drop", [] {
                     Direct<> d(6, 1);
                     CHECK(d.writer.write(1u));
                     CHECK(!d.writer.write(2u));
                     CHECK(d.writer.backpressure_stats().dropped_records == 1);
                     tick(d.service);
                     CHECK(d.writer.write(2u));
                     d.drain();
                     CHECK(d.receiver.messages.size() == 2);
                     CHECK(d.writer.backpressure_stats().blocked_events == 0);
                 }});
    c.push_back({"W_pressure_block_timeout", [] {
                     Direct<> d(6, 1,
                                {.mode = BackpressureMode::Block,
                                 .spin_count = 0,
                                 .wait_get_timeout = 10ms});
                     d.service.prepare_start();
                     CHECK(d.writer.write(1u));
                     CHECK(!d.writer.write(2u));
                     CHECK(d.writer.backpressure_stats().blocked_events == 1);
                     CHECK(d.writer.backpressure_stats().dropped_records == 1);
                     tick(d.service);
                     CHECK(d.writer.write(2u));
                     d.drain();
                     CHECK(d.receiver.messages.size() == 2);
                 }});
    c.push_back({"W_pressure_block_resume", [] {
                     WriterFixture pool(6, 1, 1,
                                      {.mode = BackpressureMode::Block,
                                       .spin_count = 0,
                                       .wait_get_timeout = 2s});
                     Writer<> writer(&pool.queue, 0, pool.backpressure);
                     Receiver rx;
                     rx.reject = true;
                     ReadWorker reader({}, rx);
                     CHECK(reader.add_writer(&writer));
                     CHECK(reader.start());
                     std::atomic<bool> accepted{false};
                     brq_support::JoiningThread producer([&] {
                         // Single-producer constraint: every writer operation must come from the same thread.
                         const bool first = writer.write(1u);
                         const bool second = writer.write(2u);
                         accepted = first && second;
                         writer.finish();
                     });
                     const auto deadline = std::chrono::steady_clock::now() + 1s;
                     while (writer.backpressure_stats().blocked_events == 0 &&
                            std::chrono::steady_clock::now() < deadline)
                         std::this_thread::yield();
                     const bool blocked = writer.backpressure_stats().blocked_events == 1;
                     rx.reject = false;
                     reader.notify_memory_available();
                     producer.join();
                     reader.stop();
                     CHECK(blocked);
                     CHECK(accepted.load());
                     CHECK(rx.good && rx.messages.size() == 2);
                     CHECK(writer.backpressure_stats().dropped_records == 0);
                     CHECK(pool.all_released());
                 }});
    for (std::string k : {"fast", "timeout"})
        c.push_back({"R_stop_" + k, [k] {
                         WriterFixture pool(64, 1, 1);
                         Writer<> writer(&pool.queue, 0, pool.backpressure);
                         Receiver rx;
                         rx.reject = true;
                         ReadConfig cfg;
                         cfg.finish_timeout = 10ms;
                         ReadWorker worker(cfg, rx);
                         CHECK(worker.add_writer(&writer));
                         CHECK(writer.write(42u));
                         writer.finish();
                         CHECK(worker.start());
                         worker.stop(k == "fast" ? ReadStopMode::fast : ReadStopMode::slow);
                         CHECK(rx.messages.empty());
                         CHECK(!worker.running());
                         if (k == "timeout")
                             CHECK(worker.error_code() == ReadError::finish_timeout);
                         rx.reject = false;
                         CHECK(worker.start());
                         worker.stop();
                         CHECK(rx.good && rx.messages.size() == 1);
                         CHECK(pool.all_released());
                     }});
    for (std::string k : {"callback", "retry"})
        c.push_back({"S_config_" + k, [k] {
                         invalid([&] {
                             if (k == "callback")
                                 ReadBlockFactory bad(0, 8); // Zero block capacity is invalid.
                             else
                                 ReadBlockFactory bad(64, 0); // Zero frame capacity is invalid.
                         });
                     }});
}
} // namespace ft
