#include "../../support/writer_fixture.hpp"
#include "../../support/warm_pool_test_support.hpp"
#include "../support.hpp"
namespace {
using namespace ft;
using Queue = WriterQueue;
template<class Bytes> unsigned value(const Bytes& bytes) {
    CHECK(bytes.size() == 6);
    CHECK(bytes.data()[0] == std::byte{4} && bytes.data()[1] == std::byte{0});
    unsigned n; std::memcpy(&n, bytes.data() + 2, 4); return n;
}
// Current becomes visible on the consumer side after acknowledging the start record.
template<class W> void acknowledge_start(W& writer) {
    auto& queue = writer.handoff_queue();
    auto start = queue.try_claim();
    CHECK(start && !start.get()); queue.finish(start);
}
// Only finished claims can return a source block. Reading current just advances its cursor.
std::vector<unsigned> drain(Queue& queue) {
    std::vector<unsigned> result;
    while (auto claim = queue.try_claim()) {
        if (!claim.get()) { queue.finish(claim); continue; }
        auto bytes = claim->readable();
        for (std::size_t pos = 0; pos < bytes.size(); pos += 6) result.push_back(value(bytes.subspan(pos, 6)));
        CHECK(claim->consume(bytes.size())); queue.finish(claim);
    }
    return result;
}
void finished_before_read() {
    WriterFixture mgr(64,1,1); Writer<> writer(&mgr.queue, 0, mgr.backpressure); acknowledge_start(writer);
    CHECK(writer.write(11u) && writer.write(22u)); writer.finish();
    CHECK(!mgr.handoff_queue().current_producer() && mgr.in_use_count()==1);
    CHECK(drain(mgr.handoff_queue()) == std::vector<unsigned>({11,22}));
    CHECK(mgr.all_released());
}
void unread_tail() {
    WriterFixture mgr(64,1,1); Writer<> writer(&mgr.queue, 0, mgr.backpressure); acknowledge_start(writer);
    auto& queue=mgr.handoff_queue();
    CHECK(writer.write(11u)); auto current=queue.current();
    CHECK(current && value(current->readable())==11 && current->consume(6));
    CHECK(writer.write(22u)); writer.finish();
    CHECK(!queue.current_producer());
    CHECK(drain(queue)==std::vector<unsigned>{22} && mgr.all_released());
}
void consume_before_handoff() {
    WriterFixture mgr(64,1,1); Writer<> writer(&mgr.queue, 0, mgr.backpressure); acknowledge_start(writer);
    auto& queue=mgr.handoff_queue();
    CHECK(writer.write(42u)); auto current=queue.current(); CHECK(current->consume(6));
    CHECK(!current->finish() && mgr.in_use_count()==1);
    writer.finish(); CHECK(mgr.in_use_count()==1);
    CHECK(drain(queue).empty() && mgr.all_released());
}
void empty_finished() {
    WriterFixture mgr(64,1,1);
    for(unsigned i=0;i<100;++i) {
        Writer<> writer(&mgr.queue, 0, mgr.backpressure); acknowledge_start(writer); writer.finish();
        CHECK(drain(mgr.handoff_queue()).empty() && mgr.all_released());
    }
}
void claimed_generation() {
    WriterFixture mgr(6,1,1); Writer<> writer(&mgr.queue, 0, mgr.backpressure); acknowledge_start(writer);
    auto& queue=mgr.handoff_queue(); auto loan=queue.current();
    const auto generation=loan->sequence_;
    CHECK(writer.write(71u) && !writer.write(72u));
    auto claim=queue.try_claim(); CHECK(value(claim->readable())==71);
    CHECK(!writer.write(72u) && !queue.callbacks().can_finish(*claim));
    CHECK(claim->consume(6)); queue.finish(claim);
    CHECK(writer.write(72u) && writer.source_block()==loan.get());
    CHECK(writer.source_block()->sequence_!=generation);
    writer.finish(); CHECK(drain(queue)==std::vector<unsigned>{72} && mgr.all_released());
}
void callback_reuse() {
    WriterFixture mgr(6,1,1); Writer<> writer(&mgr.queue, 0, mgr.backpressure); acknowledge_start(writer);
    struct Context { WriterFixture& pool; Writer<>& writer; unsigned calls=0; } c{mgr,writer};
    struct Callback {
        Context& context;
        void begin_read_complete(std::int64_t) noexcept {}
        bool read_complete(SpscWriteBlock* block) noexcept {
            const auto bytes = block->readable();
            return read_current(block, bytes.data(), bytes.size());
        }
        void end_read_complete() noexcept {}
        void begin_read_current(std::int64_t) noexcept {}
        bool read_current(SpscWriteBlock*, const std::byte* ptr, std::size_t size) noexcept {
            CHECK(value(std::span(ptr, size)) == 11 + context.calls);
            CHECK(!context.pool.all_released());
            CHECK(!context.writer.write(12u)); ++context.calls; return true;
        }
        void end_read() noexcept {}
    } callback{c};
    AutoResetEvent event; ReadService service({},callback,event);
    CHECK(service.add_writer(&writer,false)); CHECK(writer.write(11u) && !writer.write(12u));
    tick(service); CHECK(c.calls==1 && mgr.all_released());
    CHECK(writer.write(12u)); writer.finish(); tick(service);
    CHECK(c.calls==2 && mgr.all_released() && service.stats().saved_data_bytes==12);
}
void rejected_finished() {
    WriterFixture mgr(6,1,1); Writer<> writer(&mgr.queue, 0, mgr.backpressure); acknowledge_start(writer); Receiver receiver;
    receiver.reject=true; ReadConfig config; config.finish_timeout=5ms; config.idle_spin_count=0;
    ReadWorker worker(config,receiver);
    CHECK(worker.add_writer(&writer)); CHECK(writer.write(81u)); writer.finish();
    CHECK(worker.start()); worker.stop();
    CHECK(worker.error_code()==ReadError::finish_timeout && mgr.in_use_count()==1);
    CHECK(worker.pending_finished_count()==1 && receiver.messages.empty());
    receiver.reject=false; CHECK(worker.start()); worker.stop();
    CHECK(receiver.good && receiver.messages.size()==1 && mgr.all_released());
}
void finish_read_race() {
    WriterFixture mgr(64,1,1); auto& queue=mgr.handoff_queue();
    for(unsigned round=0;round<400;++round) {
        Writer<> writer(&mgr.queue, 0, mgr.backpressure); acknowledge_start(writer);
        std::barrier start(2); std::atomic<bool> initial{false}, done{false};
        std::thread publish([&] {
            // Both writes belong to the same producer thread, including the initial record.
            CHECK(writer.write(round)); initial.store(true,std::memory_order_release);
            start.arrive_and_wait(); CHECK(writer.write(round+1000)); writer.finish(); done.store(true,std::memory_order_release);
        });
        while (!initial.load(std::memory_order_acquire)) std::this_thread::yield();
        auto current=queue.current(); CHECK(value(current->readable())==round && current->consume(6));
        start.arrive_and_wait(); unsigned received=0;
        while(!done.load(std::memory_order_acquire)) {
            if(auto c=queue.current()) { auto bytes=c->readable(); if(!bytes.empty()) {
                CHECK(value(bytes)==round+1000 && c->consume(bytes.size())); ++received;
            }}
        }
        publish.join(); auto tail=drain(queue);
        for(auto n:tail) { CHECK(n==round+1000); ++received; }
        CHECK(received==1 && mgr.all_released());
    }
}
void queued_finish_reuse() {
    WriterFixture mgr(6,1,1); Writer<> writer(&mgr.queue, 0, mgr.backpressure); acknowledge_start(writer); auto& queue=mgr.handoff_queue();
    CHECK(writer.write(101u)); auto current=queue.current(); CHECK(current->consume(6));
    CHECK(!writer.write(102u) && queue.pending_count()==1);
    for(unsigned i=0;i<16;++i) CHECK(!queue.current_producer());
    CHECK(drain(queue).empty() && mgr.all_released());
    CHECK(writer.write(102u)); writer.finish();
    CHECK(drain(queue)==std::vector<unsigned>{102});
}
void empty_interleaved() {
    WriterFixture mgr(6,6,6); Receiver receiver; AutoResetEvent event;
    ReadService service({},receiver,event);
    CHECK(service.add_queue(&mgr.handoff_queue(),7));
    for(unsigned round=0;round<3;++round) {
        for(unsigned i=0;i<6;++i) { Writer<> writer(&mgr.queue, 7, mgr.backpressure); if(round==1 && (i&1)) CHECK(writer.write(i)); writer.finish(); }
        tick(service); CHECK(mgr.all_released());
    }
    CHECK(receiver.messages.size()==3 && service.stats().saved_finished_ranges==18 && service.stats().saved_data_bytes==18);
}
void repeated_current() {
    constexpr unsigned count=1300; WriterFixture mgr(count*6,1,1); Writer<> writer(&mgr.queue, 0, mgr.backpressure); acknowledge_start(writer);
    auto& queue=mgr.handoff_queue();
    for(unsigned i=0;i<count;++i) {
        for(unsigned n=0;n<4;++n) CHECK(queue.current()->readable().empty());
        CHECK(writer.write(i)); auto current=queue.current();
        CHECK(value(current->readable())==i && current->consume(6) && mgr.in_use_count()==1);
    }
    writer.finish(); CHECK(drain(queue).empty() && mgr.all_released());
}
void empty_callback() {
    WriterFixture mgr(6,1,1); Writer<> writer(&mgr.queue, 0, mgr.backpressure); acknowledge_start(writer); Receiver receiver; AutoResetEvent event;
    ReadService service({},receiver,event); CHECK(service.add_writer(&writer,false));
    writer.finish(); tick(service);
    CHECK(receiver.calls==0 && service.stats().saved_finished_ranges==1 && mgr.all_released());
}
void direct_handoff() {
    WriterFixture mgr(6,2,2); Writer<> writer(&mgr.queue, 0, mgr.backpressure); acknowledge_start(writer); auto& queue=mgr.handoff_queue();
    auto* first=writer.source_block(); CHECK(writer.write(401u) && writer.write(402u)); auto* second=writer.source_block();
    writer.finish(); auto claim=queue.try_claim(); CHECK(claim.get()==first && !queue.current_producer());
    queue.cancel_claim(claim);
    claim=queue.try_claim(); CHECK(claim.get()==first && value(claim->readable())==401);
    CHECK(claim->consume(6)); queue.finish(claim);
    claim=queue.try_claim(); CHECK(claim.get()==second && value(claim->readable())==402);
    CHECK(claim->consume(6)); queue.finish(claim); CHECK(mgr.all_released());
}
void current_handoff() {
    WriterFixture mgr(6,2,2); Writer<> writer(&mgr.queue, 9, mgr.backpressure); acknowledge_start(writer); Receiver receiver; AutoResetEvent event;
    ReadService service({},receiver,event); CHECK(service.add_writer(&writer,false));
    for(unsigned n:{501u,502u}) { CHECK(writer.write(n)); for(unsigned i=0;i<3;++i) tick(service); CHECK(mgr.in_use_count()==1); }
    writer.finish(); tick(service);
    CHECK(receiver.messages.size()==2 && service.stats().saved_data_bytes==12 && mgr.all_released());
}
void discard_unread_tail() {
    WriterFixture mgr(64,1,1); Writer<> writer(&mgr.queue, 0, mgr.backpressure); acknowledge_start(writer);
    CHECK(writer.write(701u)); auto current=mgr.handoff_queue().current(); CHECK(current->consume(6));
    CHECK(writer.write(702u)); writer.finish(); // Manager discards unread tail after both endpoints stop.
}
void buffered_retry_without_release_notify() {
    WriterFixture mgr(6, 2, 2);
    Writer<> writer(&mgr.queue, 0, mgr.backpressure); acknowledge_start(writer);
    ReadBlockPool pool(1, 1, ReadBlockFactory{6, 1});
    OutputQueue output(pool, 2);
    StoreSaver adapter(output);
    ReadConfig config;
    config.idle_spin_count = 0;
    config.idle_rescan_interval = config.idle_rescan_max_interval = 10s;
    config.work_wait_min_interval = 1ms;
    ReadWorker worker(config, adapter);
    CHECK(worker.add_writer(&writer));
    CHECK(writer.write(601u) && writer.write(602u));
    writer.finish();
    CHECK(worker.start());
    const auto wait = [](auto&& predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!predicate()) {
            CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
    };
    OutputQueue::Loan first, second;
    wait([&] { first = output.try_receive(); return bool(first); });
    CHECK(first->frames().size() == 1 && value(std::span(first->frames()[0].data, first->frames()[0].size)) == 601);
    std::this_thread::sleep_for(20ms);
    CHECK(!output.try_receive());
    CHECK(output.finish(first)); // Deliberately omit notify_memory_available: timed retry must work.
    wait([&] { second = output.try_receive(); return bool(second); });
    CHECK(second->frames().size() == 1 && value(std::span(second->frames()[0].data, second->frames()[0].size)) == 602);
    CHECK(output.finish(second));
    worker.stop(); output.close();
    CHECK(worker.error_code() == ReadError::none && adapter.error_code() == ToReadBlockError::none);
    CHECK(mgr.all_released() && pool.all_released());
    CHECK(worker.stats().saved_data_bytes == 12);
}
void write_finished_holds_block() {
    auto pool = make_block_pool<TestBlockPool<WriterBlock<>>>(64, 1, 1);
    for (unsigned round = 0; round < 2; ++round) {
        auto* block = pool.get().get();
        std::memset(block->buf_, 0x5a, 8);
        block->write_ptr_.store(block->buf_ + 8, std::memory_order_release);
        auto slice = block->allocate_read_slice();
        slice.release();
        CHECK(block->is_alloc_finish() && block->is_all_released());
        CHECK(pool.free_resource_count() == 0 && !block->is_free());
        block->to_write_finished();
        CHECK(pool.free_resource_count() == 0 && !pool.get().get());
        block->to_leave_writer();
        CHECK(pool.all_released() && pool.in_use_count() == 0);
        CHECK(pool.all_released() && pool.in_use_count() == 0);
    }
}
} // namespace
int main(int argc,char** argv) {
    const ft::Cases cases{
        {"finished_before_read",finished_before_read}, {"unread_tail",unread_tail},
        {"consume_before_handoff",consume_before_handoff}, {"empty_finished",empty_finished},
        {"claimed_generation",claimed_generation}, {"callback_reuse",callback_reuse},
        {"rejected_finished",rejected_finished}, {"finish_read_race",finish_read_race},
        {"queued_finish_reuse",queued_finish_reuse}, {"write_finished_holds_block",write_finished_holds_block},
        {"empty_interleaved",empty_interleaved}, {"repeated_current",repeated_current},
        {"empty_callback",empty_callback}, {"direct_handoff",direct_handoff}, {"current_handoff",current_handoff},
        {"buffered_retry_without_release_notify",buffered_retry_without_release_notify}, {"discard_unread_tail",discard_unread_tail}};
    try {
        CHECK(argc==2);
        for(const auto& test:cases) if(test.id==argv[1]) { test.run(); std::cout<<"PASS "<<test.id<<'\n'; return 0; }
        throw std::runtime_error("unknown lifecycle case");
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
