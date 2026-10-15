#include "../../../support/joining_thread.hpp"
#include "common.hpp"
namespace ct {
struct CopiedFrame {
    int thread_id;
    Bytes bytes;
};
struct BatchWork {
    std::vector<CopiedFrame> copied;
    OutputQueue::Loan loan;
    bool wait_next_write = false;
};
struct PipelineReceiver {
    const std::vector<std::vector<Bytes>>& expected;
    Failure& failure;
    bool asynchronous, reject, force_overlap;
    OutputQueue* output = nullptr;
    Channel<BatchWork> channel{8};
    std::vector<unsigned> next;
    std::vector<std::vector<unsigned>> seen;
    std::mutex seen_mutex;
    std::vector<brq_support::JoiningThread> workers;
    std::atomic<unsigned> accepted{0}, asynchronous_batches{0}, cross_round_checks{0};
    unsigned calls = 0, rejections = 0;
    PipelineReceiver(const std::vector<std::vector<Bytes>>& e, Failure& f, bool async, bool r, bool overlap)
        : expected(e), failure(f), asynchronous(async), reject(r), force_overlap(overlap), next(e.size()), seen(e.size()) {
        for (std::size_t i = 0; i < e.size(); ++i) seen[i].resize(e[i].size());
        if (asynchronous) for (unsigned i = 0; i < 4; ++i) workers.emplace_back([&, i] {
            while (auto work = channel.pop()) {
                failure.run([&] {
                    if (work->wait_next_write) {
                        const auto deadline = Clock::now() + 10s;
                        while (accepted.load() < 2 && !failure.failed.load()) {
                            CHECK(Clock::now() < deadline); std::this_thread::yield();
                        }
                        cross_round_checks.fetch_add(1);
                    }
                    for (unsigned n = 0; n < (i + 1) * 3; ++n) std::this_thread::yield();
                    validate_work(*work);
                    asynchronous_batches.fetch_add(1);
                });
                if (work->loan) CHECK(output->finish(work->loan));
            }
        });
    }
    void finish() {
        channel.close();
        for (auto& worker : workers) if (worker.joinable()) worker.join();
    }
    ~PipelineReceiver() { finish(); }
    void validate(int id, const std::byte* data, std::size_t size, bool ingress) {
        CHECK(id >= 0 && static_cast<std::size_t>(id) < expected.size());
        for (std::size_t at = 0; at < size;) {
            CHECK(size - at >= 2);
            const auto length = std::to_integer<unsigned>(data[at]) + (std::to_integer<unsigned>(data[at + 1]) << 8);
            at += 2; CHECK(length >= 16 && length <= size - at);
            const auto writer = get32(data + at), seq = get32(data + at + 4);
            CHECK(writer == static_cast<unsigned>(id) && seq < expected[writer].size());
            equal_bytes(data + at, length, expected[writer][seq], "pipeline payload");
            if (ingress) { CHECK(seq == next[writer]); ++next[writer]; }
            else { std::lock_guard lock(seen_mutex); CHECK(++seen[writer][seq] == 1); }
            at += length;
        }
    }
    void validate_work(const BatchWork& work) {
        if (work.loan) for (const auto& frame : work.loan->frames()) validate(frame.thread_id, frame.data, frame.size, false);
        else for (const auto& frame : work.copied) validate(frame.thread_id, frame.bytes.data(), frame.bytes.size(), false);
    }
    bool receive(const DataBatch& batch, OutputQueue::Loan* loan = nullptr) {
        ++calls;
        if (reject && calls % 7 == 1) { ++rejections; return false; }
        std::size_t total = 0;
        BatchWork work;
        for (const auto& frame : batch.frames) {
            total += frame.size; validate(frame.thread_id, frame.data, frame.size, true);
            if (!loan) work.copied.push_back({frame.thread_id, {frame.data, frame.data + frame.size}});
        }
        CHECK(total == batch.data_size);
        if (loan) work.loan = std::exchange(*loan, {});
        work.wait_next_write = force_overlap && accepted.load() == 0;
        if (!asynchronous || !channel.push(work)) {
            validate_work(work);
            if (work.loan) CHECK(output->finish(work.loan));
        }
        accepted.fetch_add(1);
        return true;
    }
    void begin_read_complete(std::int64_t) noexcept {}
    bool read_complete(SpscWriteBlock* block) noexcept {
        const auto bytes = block->readable(); return read_current(block, bytes.data(), bytes.size());
    }
    void end_read_complete() noexcept {}
    void begin_read_current(std::int64_t) noexcept {}
    bool read_current(SpscWriteBlock* block, const std::byte* data, std::size_t size) noexcept {
        bool accepted_batch = false;
        failure.run([&] {
            const DataFrame frame{block->source_id(), data, size, 0};
            accepted_batch = receive({std::span(&frame, 1), size});
        });
        return accepted_batch;
    }
    void end_read() noexcept {}
    void verify() {
        failure.check();
        if (asynchronous) CHECK(asynchronous_batches.load() > 0);
        if (force_overlap) CHECK(cross_round_checks.load() == 1);
        if (reject) CHECK(rejections > 0);
        for (std::size_t w = 0; w < expected.size(); ++w) {
            CHECK(next[w] == expected[w].size());
            for (auto count : seen[w]) CHECK(count == 1);
        }
    }
};

template<unsigned TPublish>
void pipeline(bool buffered, const std::string& style, unsigned producers, bool large = false, bool active = false) {
    const unsigned count = active ? 257 : (large ? 129 : 1537);
    const std::size_t block = large ? 65537 : 1024;
    std::vector<std::vector<Bytes>> expected(producers);
    for (unsigned w = 0; w < producers; ++w) for (unsigned n = 0; n < count; ++n) {
        const auto size = large ? 65534 + (n % 2) : std::array<std::size_t, 7>{16,17,31,64,127,257,509}[n % 7];
        expected[w].push_back(message(w, n, size));
    }
    Failure failure;
    std::vector<std::unique_ptr<WriterFixture>> pools;
    std::vector<std::unique_ptr<Writer<TPublish>>> writers;
    for (unsigned w = 0; w < producers; ++w) {
        pools.push_back(std::make_unique<WriterFixture>(block, 4, 4));
        writers.push_back(std::make_unique<Writer<TPublish>>(&pools.back()->queue, static_cast<int>(w), pools.back()->backpressure));
    }
    PipelineReceiver receiver(expected, failure, style != "sync", style == "reject", active);
    ReadConfig config; config.finish_timeout = 20s; config.idle_rescan_interval = 100us; config.idle_rescan_max_interval = 500us;
    const auto run_writers = [&](auto& reader) {
        for (auto& writer : writers) CHECK(reader.add_writer(writer.get()));
        CHECK(reader.start());
        std::barrier gate(static_cast<std::ptrdiff_t>(producers + 1));
        std::vector<brq_support::JoiningThread> threads;
        for (unsigned w = 0; w < producers; ++w) threads.emplace_back([&, w] {
            gate.arrive_and_wait();
            failure.run([&] {
                const auto deadline = Clock::now() + 30s;
                for (unsigned n = 0; n < count && !failure.failed.load(); ++n) {
                    auto input = expected[w][n];
                    while (!writers[w]->write_mem(input.data(), input.size())) {
                        CHECK(Clock::now() < deadline && !failure.failed.load()); std::this_thread::yield();
                    }
                    std::fill(input.begin(), input.end(), std::byte{0xdd});
                    if (active || n % 19 == 0) writers[w]->flush();
                    if (active) while (receiver.accepted.load() < n + 1 && !failure.failed.load()) {
                        CHECK(Clock::now() < deadline); std::this_thread::yield();
                    }
                    if ((n + w) % 13 == 0) std::this_thread::yield();
                }
            });
            writers[w]->finish();
        });
        gate.arrive_and_wait(); for (auto& thread : threads) thread.join();
        reader.stop(); CHECK(reader.error_code() == ReadError::none);
    };
    if (buffered) {
        ReadBlockPool pool(8, 8, ReadBlockFactory{block, 8});
        OutputQueue output(pool, 8); StoreSaver saver(output); receiver.output = &output;
        ReadWorker reader(config, saver);
        brq_support::JoiningThread consumer([&] {
            OutputQueue::Loan loan;
            while (true) {
                if (!loan) loan = output.try_receive();
                if (loan) {
                    failure.run([&] { (void)receiver.receive(to_data_batch(*loan), &loan); });
                    if (failure.failed.load() && loan) CHECK(output.finish(loan));
                } else if (output.is_closed()) break;
                else std::this_thread::yield();
            }
        });
        run_writers(reader); output.close(); consumer.join(); receiver.finish();
        CHECK(pool.all_released() && saver.error_code() == ToReadBlockError::none);
    } else {
        ReadWorker reader(config, receiver); run_writers(reader); receiver.finish();
    }
    receiver.verify();
    for (auto& pool : pools) CHECK(pool->all_released() && pool->handoff_queue().pending_count() == 0);
    std::cout << "verified messages=" << count * producers << " callbacks=" << receiver.calls
              << " rejected=" << receiver.rejections << " async batches=" << receiver.asynchronous_batches << '\n';
}
void pipeline_cases(Cases& cases) {
    for (bool buffered : {false,true}) {
        const std::string mode = buffered ? "buffered" : "direct";
        for (std::string style : {"sync","async","reject"}) for (unsigned p : {1u,4u}) for (unsigned pub : {1u,32u})
            cases.push_back({"pipe_" + mode + "_" + style + "_" + std::to_string(p) + "_" + std::to_string(pub), [=] {
                if (pub == 1) pipeline<1>(buffered,style,p); else pipeline<32>(buffered,style,p);
            }});
        for (unsigned p : {1u,4u}) cases.push_back({"large_" + mode + "_" + std::to_string(p), [=] { pipeline<1>(buffered,"async",p,true); }});
        cases.push_back({"active_" + mode, [=] { pipeline<32>(buffered,"async",1,false,true); }});
    }
}
}
