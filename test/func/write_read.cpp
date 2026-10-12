#include "../writer_fixture.hpp"
#include "support.hpp"
namespace ft {
void write_read_cases(Cases &c) {
    for (auto n : {0u, 1u, 2u, 3u, 4u, 31u, 32u, 255u, 256u, 1023u, 1024u, 65534u, 65535u})
        c.push_back({"W_len_" + std::to_string(n), [n] {
                         Direct<> d(n + 2, 1);
                         auto src = payload(n);
                         auto expected = src;
                         CHECK(d.writer.write_mem(src.data(), static_cast<std::uint16_t>(src.size())));
                         auto *p = d.writer.source_block()->buf_;
                         CHECK(std::to_integer<unsigned>(p[0]) == (n & 255));
                         CHECK(std::to_integer<unsigned>(p[1]) == (n >> 8));
                         std::fill(src.begin(), src.end(), std::byte{0});
                         d.drain();
                         CHECK(d.receiver.messages == std::vector<Message>{{3, expected}});
                         CHECK(d.pool.all_released());
                     }});
    for (std::string k : {"null", "oversize", "block"})
        c.push_back({"W_reject_" + k, [k] {
                         Direct<1, 4> d(16, 2);
                         CHECK(d.writer.write(1u));
                         auto *before = d.writer.published_write_ptr();
                         auto bytes = payload(32);
                         bool result = true;
                         if (k == "null")
                             result = d.writer.write_mem(nullptr, 4);
                         if (k == "oversize")
                             result = d.writer.write_mem(bytes.data(), (std::numeric_limits<std::uint16_t>::max)());
                         if (k == "block")
                             result = d.writer.write_mem(bytes.data(), 32);
                         CHECK(!result);
                         CHECK(d.writer.published_write_ptr() == before);
                         CHECK(d.writer.write(2u));
                         d.drain();
                         CHECK(d.receiver.messages.size() == 2);
                         unsigned a = 0, b = 0;
                         std::memcpy(&a, d.receiver.messages[0].bytes.data(), 4);
                         std::memcpy(&b, d.receiver.messages[1].bytes.data(), 4);
                         CHECK(a == 1 && b == 2);
                     }});
    c.push_back({"W_short_payload", [] {
        Direct<> d(16, 1);
        const auto bytes = payload(3);
        CHECK(d.writer.write_mem(bytes.data(), 3)); d.drain();
        CHECK(d.receiver.messages == std::vector<Message>{{3, bytes}});
    }});
    auto pub = [&]<unsigned N>() {
        c.push_back(
            {"W_publish_" + std::to_string(N), [] {
                 Direct<N> d(512, 2);
                 auto *begin = d.writer.published_write_ptr();
                 for (unsigned i = 0; i < N; ++i) {
                     CHECK(d.writer.write(i));
                     CHECK(d.writer.published_write_ptr() == begin + (i + 1 == N ? N * 6 : 0));
                 }
                 CHECK(d.writer.write(99u));
                 CHECK(d.writer.published_write_ptr() == begin + (N + (N == 1 ? 1 : 0)) * 6);
                 d.writer.flush();
                 CHECK(d.writer.published_write_ptr() == begin + (N + 1) * 6);
                 d.drain();
                 CHECK(d.receiver.messages.size() == N + 1);
             }});
    };
    pub.operator()<1>();
    pub.operator()<2>();
    pub.operator()<32>();
    c.push_back({"W_pack", [] {
                     Direct<> d;
                     std::uint8_t a = 7;
                     std::uint64_t b = 0xfedcba9876543210ULL;
                     std::int16_t e = -123;
                     std::vector<std::byte> expected(sizeof(a) + sizeof(b) + sizeof(e));
                     std::memcpy(expected.data(), &a, sizeof(a));
                     std::memcpy(expected.data() + sizeof(a), &b, sizeof(b));
                     std::memcpy(expected.data() + sizeof(a) + sizeof(b), &e, sizeof(e));
                     CHECK(d.writer.write(a, b, e));
                     d.drain();
                     CHECK(d.receiver.messages == std::vector<Message>{{3, expected}});
                 }});
    c.push_back({"W_finish", [] {
                     Direct<> d;
                     CHECK(d.writer.write(7u));
                     d.writer.finish();
                     d.writer.finish();
                     d.writer.flush();
                     CHECK(!d.writer.write(8u));
                     CHECK(!d.writer.write_mem(nullptr, 0));
                     d.drain();
                     CHECK(d.receiver.messages.size() == 1);
                     CHECK(!d.writer.source_block());
                 }});
    c.push_back({"R_active", [] {
                     Direct<> d;
                     CHECK(d.writer.write(7u));
                     tick(d.service);
                     CHECK(d.receiver.messages.size() == 1);
                     CHECK(d.writer.write(8u));
                     d.drain();
                     CHECK(d.receiver.messages.size() == 2);
                     CHECK(d.service.stats().saved_data_bytes == 12);
                 }});
    c.push_back({"R_reject", [] {
                     Direct<> d;
                     d.receiver.reject = true;
                     CHECK(d.writer.write(7u));
                     tick(d.service);
                     tick(d.service);
                     auto *r = d.writer.source_block();
                     const auto read = r->read_size();
                     CHECK(r->readable().size() == 6 && r->read_pos() == 0);
                     for (int i = 0; i < 100; ++i)
                         tick(d.service);
                     CHECK(r->read_size() == read);
                     CHECK(r->readable().size() == 6 && r->read_pos() == 0);
                     CHECK(d.writer.write(8u));
                     d.receiver.reject = false;
                     d.drain();
                     CHECK(d.receiver.messages.size() == 2);
                     CHECK(d.service.stats().memory_full_events > 0);
                 }});
    c.push_back({"R_retain", [] {
                     // Fast callbacks copy data they need after returning true.
                     Direct<> d(6, 1);
                     CHECK(d.writer.write(17u));
                     d.drain();
                     CHECK(d.pool.handoff_queue().empty() && d.pool.all_released());
                     unsigned n = 0;
                     std::memcpy(&n, d.receiver.messages[0].bytes.data(), 4);
                     CHECK(n == 17);
                 }});
    for (std::string k : {"cancel", "deadline"})
        c.push_back({"R_" + k, [k] {
                         Direct<> d;
                         CHECK(d.writer.write(9u));
                         d.writer.finish();
                         std::atomic<bool> cancel{k == "cancel"};
                         const auto deadline = k == "deadline"
                                                   ? AutoResetEvent::Clock::now() - 1ms
                                                   : AutoResetEvent::Clock::time_point::max();
                         (void)d.service.read_once(true, cancel, deadline);
                         CHECK(d.receiver.messages.empty());
                         d.drain();
                         CHECK(d.receiver.messages.size() == 1);
                     }});
    for (std::string k :
         {"nullwriter", "nullqueue", "queue_negative", "duplicate", "duplicateid", "negative", "running"})
        c.push_back({"R_reg_" + k, [k] {
                         WriterFixture a(64, 1, 2), b(64, 1, 2);
                         Writer<> w(&a.queue, k == "negative" ? -1 : 1, a.backpressure), v(&b.queue, 1, b.backpressure);
                         Receiver rx;
                         AutoResetEvent e;
                         ReadService s({}, rx, e);
                         bool ok = false;
                         ReadError expected = ReadError::invalid_registration;
                         std::size_t count = 0;
                         if (k == "nullwriter") {
                             ok = s.add_writer(nullptr, false);
                             expected = ReadError::null_dependency;
                         }
                         if (k == "nullqueue") {
                             ok = s.add_queue(nullptr, 1);
                             expected = ReadError::null_dependency;
                         }
                         if (k == "queue_negative") {
                             ok = s.add_queue(&b.queue, -1);
                             expected = ReadError::invalid_registration;
                         }
                         if (k == "negative")
                             ok = s.add_writer(&w, false);
                         if (k == "running")
                             ok = s.add_writer(&w, true);
                         if (k == "duplicate" || k == "duplicateid") {
                             CHECK(s.add_writer(&w, false));
                             count = 1;
                             ok = k == "duplicate" ? s.add_writer(&w, false)
                                                   : s.add_writer(&v, false);
                             expected = ReadError::duplicate_writer;
                         }
                         CHECK(!ok);
                         CHECK(s.error_code() == expected);
                         CHECK(s.writer_count() == count);
                         CHECK(s.stats().error_count == 1);
                     }});
    for (std::string k : {"scan0", "max0", "finish0", "wait0", "retry0", "callback", "writers"})
        c.push_back({"R_config_" + k, [k] {
                         ReadConfig cfg;
                         if (k == "scan0")
                             cfg.idle_rescan_interval = 0ns;
                         if (k == "max0")
                             cfg.idle_rescan_max_interval = 0ns;
                         if (k == "finish0")
                             cfg.finish_timeout = 0ms;
                         if (k == "wait0")
                             cfg.work_wait_min_interval = 0ms;
                         if (k == "retry0")
                             cfg.finish_retry_interval = 0ms;
                         WriterFixture p(64, 1, 1);
                         Writer<> w(&p.queue, 0, p.backpressure);
                         Receiver rx;
                         ReadWorker worker(cfg, rx);
                         if (k == "callback") {
                             CHECK(worker.add_writer(&w));
                             CHECK(worker.start());
                             CHECK(worker.running());
                             worker.stop();
                             CHECK(!worker.running());
                             return;
                         }
                         if (k != "writers") {
                             CHECK(worker.add_writer(&w));
                         }
                         CHECK(!worker.start());
                         CHECK(!worker.running());
                         CHECK(worker.error_code() == ReadError::invalid_start_state);
                     }});
    for (unsigned seed : {1u, 7u, 42u, 20260925u})
        c.push_back({"M_seed_" + std::to_string(seed), [seed] {
                         Direct<32> d(64, 2);
                         std::mt19937 rng(seed);
                         std::vector<Message> model;
                         for (unsigned step = 0; step < 1000; ++step) {
                             switch (rng() % 4) {
                             case 0:
                             case 1: {
                                 auto v = payload(rng() % 31, step);
                                 if (d.writer.write_mem(v.data(), static_cast<std::uint16_t>(v.size())))
                                     model.push_back({3, v});
                                 break;
                             }
                             case 2:
                                 d.writer.flush();
                                 break;
                             case 3:
                                 d.receiver.reject = (rng() % 3 == 0);
                                 tick(d.service);
                                 break;
                             }
                             CHECK(d.receiver.good);
                         }
                         d.receiver.reject = false;
                         d.drain();
                         CHECK(d.receiver.messages == model);
                         CHECK(d.pool.all_released());
                     }});
    c.push_back({"U_signal", [] {
                     AutoResetEvent e;
                     CHECK(!e.try_wait());
                     e.set();
                     e.set();
                     CHECK(e.try_wait());
                     CHECK(!e.try_wait());
                     e.set();
                     e.reset();
                     CHECK(!e.try_wait());
                     e.set();
                     CHECK(e.wait_for(1s));
                     CHECK(!e.try_wait());
                 }});
    c.push_back({"U_cancel", [] {
                     AutoResetEvent e;
                     std::atomic<bool> cancel{true};
                     CHECK(!e.wait_for(1s, cancel));
                 }});
    c.push_back({"U_timeout", [] {
                     AutoResetEvent e;
                     CHECK(!e.wait_for(2ms));
                 }});
}
} // namespace ft
