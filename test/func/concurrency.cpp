#include "../../support/joining_thread.hpp"
#include "../writer_fixture.hpp"
#include "../warm_pool_test_support.hpp"
#include "../block_test_support.hpp"
#include "support.hpp"
namespace ft {
template <unsigned Pub> void pipeline(bool buffered, unsigned producers) {
    std::vector<std::unique_ptr<WriterFixture>> pools;
    std::vector<std::unique_ptr<Writer<Pub>>> writers;
    for (unsigned i = 0; i < producers; ++i) {
        pools.push_back(std::make_unique<WriterFixture>(256, 2, 2));
        writers.push_back(std::make_unique<Writer<Pub>>(&pools.back()->queue, static_cast<int>(i), pools.back()->backpressure));
    }
    Receiver rx;
    ReadConfig cfg;
    cfg.finish_timeout = 10s;
    cfg.idle_spin_count = 0;
    std::atomic<bool> failed{false};
    std::barrier gate(static_cast<std::ptrdiff_t>(producers + 1));
    std::vector<brq_support::JoiningThread> threads;
    for (unsigned id = 0; id < producers; ++id)
        threads.emplace_back([&, id] {
            gate.arrive_and_wait();
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            try {
                for (unsigned seq = 0; seq < 2001 && !failed.load(); ++seq) {
                    auto bytes = payload(std::array<std::size_t, 5>{4, 7, 19, 63, 127}[seq % 5],
                                         seq + id * 2001);
                    std::memcpy(bytes.data(), &seq, 4);
                    while (!writers[id]->write_mem(bytes.data(), static_cast<std::uint16_t>(bytes.size()))) {
                        if (std::chrono::steady_clock::now() >= deadline || failed.load()) {
                            failed = true;
                            break;
                        }
                        std::this_thread::yield();
                    }
                }
            } catch (...) {
                failed = true;
            }
            writers[id]->finish();
        });
    if (buffered) {
        ReadBlockPool out_pool(4, 16, ReadBlockFactory{256, 16});
        OutputQueue output(out_pool, 8);
        StoreSaver saver(output);
        ReadWorker<StoreSaver> reader(cfg, saver);
        brq_support::JoiningThread consumer([&] {
            for (;;) {
                auto loan = output.try_receive();
                if (loan) {
                    const auto db = to_data_batch(*loan);
                    for (const auto &frame : db.frames)
                        if (!rx.parse(frame.data, frame.size, frame.thread_id))
                            rx.good = false;
                    if (!output.finish(loan)) {
                        rx.good = false;
                        break;
                    }
                    reader.notify_memory_available();
                } else if (output.is_closed()) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });
        for (unsigned i = 0; i < producers; ++i)
            CHECK(reader.add_writer(writers[i].get()));
        CHECK(reader.start());
        CHECK(reader.start());
        gate.arrive_and_wait();
        for (auto &t : threads)
            t.join();
        reader.stop();
        output.close();
        consumer.join();
        CHECK(saver.error_code() == ToReadBlockError::none);
        CHECK(reader.error_code() == ReadError::none);
        CHECK(out_pool.all_released());
    } else {
        ReadWorker<Receiver> reader(cfg, rx);
        for (unsigned i = 0; i < producers; ++i)
            CHECK(reader.add_writer(writers[i].get()));
        CHECK(reader.start());
        CHECK(reader.start());
        gate.arrive_and_wait();
        for (auto &t : threads)
            t.join();
        reader.stop();
        CHECK(reader.error_code() == ReadError::none);
    }
    CHECK(!failed.load());
    CHECK(rx.good);
    CHECK(rx.messages.size() == producers * 2001);
    std::vector<unsigned> next(producers);
    for (auto &m : rx.messages) {
        CHECK(m.id >= 0 && static_cast<unsigned>(m.id) < producers);
        auto seq = next[static_cast<unsigned>(m.id)]++;
        auto expected = payload(std::array<std::size_t, 5>{4, 7, 19, 63, 127}[seq % 5],
                                seq + static_cast<unsigned>(m.id) * 2001);
        std::memcpy(expected.data(), &seq, 4);
        CHECK(m.bytes == expected);
    }
    for (auto n : next) {
        CHECK(n == 2001);
    }
    for (auto &p : pools) {
        CHECK(p->all_released());
        CHECK(p->handoff_queue().pending_count() == 0);
    }
}
template <class Block> void seal_race() {
    auto pool = make_block_pool<TestBlockPool<Block>>(64, 1, 1);
    std::atomic<unsigned> recycled{0};
    pool.set_release_callback(
        [](void *p) noexcept { static_cast<std::atomic<unsigned> *>(p)->fetch_add(1); }, &recycled);
    for (unsigned i = 0; i < 200; ++i) {
        auto *block = pool.get().get();
        auto slice = block->allocate(8);
        std::barrier gate(2);
        brq_support::JoiningThread t([&gate, s = std::move(slice)]() mutable {
            gate.arrive_and_wait();
            s.release();
        });
        gate.arrive_and_wait();
        finish_block(block);
        t.join();
        CHECK(pool.in_use_count() == 0);
        CHECK(recycled.load() == i + 1);
    }
    pool.set_release_callback(nullptr, nullptr);
}
void concurrency_cases(Cases &c) {
    for (bool buffered : {false, true})
        for (unsigned p : {1u, 4u})
            for (unsigned pub : {1u, 32u})
                c.push_back({std::string("C_") + (buffered ? "buffered" : "direct") + "_" +
                                 std::to_string(p) + "_" + std::to_string(pub),
                             [=] {
                                 if (pub == 1)
                                     pipeline<1>(buffered, p);
                                 else
                                     pipeline<32>(buffered, p);
                             }});
    c.push_back({"C_seal1N", [] { seal_race<ReaderBlock<>>(); }});
    c.push_back({"C_sealNN", [] { seal_race<AllocNReleaseNReaderBlock>(); }});
    c.push_back(
        {"C_allocNN", [] {
             AllocNReleaseNSlicePool pool(256, 0, 128, {.max_slices_per_block = 17});
             std::array<std::vector<AllocNReleaseNSlicePool::Slice>, 4> held;
             std::atomic<bool> good{true};
             std::barrier gate(4);
             std::vector<brq_support::JoiningThread> threads;
             for (unsigned t = 0; t < 4; ++t)
                 threads.emplace_back([&, t] {
                     gate.arrive_and_wait();
                     try {
                         for (unsigned i = 0; i < 200; ++i) {
                             auto s = pool.allocate(8, 8);
                             if (!s) {
                                 good = false;
                                 return;
                             }
                             const std::uint64_t v = t * 200 + i;
                             std::memcpy(s.data(), &v, 8);
                             held[t].push_back(std::move(s));
                         }
                     } catch (...) {
                         good = false;
                     }
                 });
             for (auto &t : threads) {
                 t.join();
             }
             CHECK(good.load());
             std::vector<std::uintptr_t> addresses;
             for (unsigned t = 0; t < 4; ++t)
                 for (unsigned i = 0; i < 200; ++i) {
                     std::uint64_t v;
                     std::memcpy(&v, held[t][i].data(), 8);
                     CHECK(v == t * 200 + i);
                     addresses.push_back(reinterpret_cast<std::uintptr_t>(held[t][i].data()));
                 }
             std::sort(addresses.begin(), addresses.end());
             CHECK(std::adjacent_find(addresses.begin(), addresses.end()) == addresses.end());
             pool.close();
             threads.clear();
             for (unsigned t = 0; t < 4; ++t)
                 threads.emplace_back([&, t] { held[t].clear(); });
             for (auto &t : threads) {
                 t.join();
             }
             CHECK(pool.all_released());
         }});
    c.push_back({"C_release1N", [] {
                     Alloc1ReleaseNSlicePool pool(1024, 1, 4);
                     std::array<std::vector<Alloc1ReleaseNSlicePool::Slice>, 4> held;
                     for (unsigned i = 0; i < 256; ++i) {
                         auto s = pool.allocate(8);
                         CHECK(s);
                         std::memset(s.data(), static_cast<int>(i & 255), 8);
                         held[i % 4].push_back(std::move(s));
                     }
                     pool.close();
                     std::barrier gate(4);
                     std::atomic<bool> good{true};
                     std::vector<brq_support::JoiningThread> ts;
                     for (unsigned t = 0; t < 4; ++t)
                         ts.emplace_back([&, t] {
                             gate.arrive_and_wait();
                             for (unsigned i = 0; i < held[t].size(); ++i) {
                                 for (unsigned j = 0; j < 8; ++j)
                                     if (held[t][i].data()[j] != std::byte((i * 4 + t) & 255))
                                         good = false;
                                 held[t][i].release();
                             }
                         });
                     for (auto &t : ts) {
                         t.join();
                     }
                     CHECK(good.load());
                     CHECK(pool.all_released());
                 }});
}
} // namespace ft
