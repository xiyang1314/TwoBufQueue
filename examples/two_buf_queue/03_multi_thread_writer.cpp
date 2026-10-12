#include "../../support/joining_thread.hpp"
// 多线程写：每个 writer 由一个独立线程写入，多个 writer 汇总到一个 TwoBufQueue。
// 每个 writer 只能被一个线程调用 write（库内部按线程校验）；用 thread_id 区分记录来源。
// 再配合 read_layer1_thread_count 开启多个第一层读线程，读写两端都是多线程。

#include "../example_support.hpp"
#include <array>
#include <cstring>
#include <thread>
using namespace TwoBufQueue;

int main() try {
    //--------------- 1. 初始化：4 个 writer（各绑一个 thread_id）+ 2 个读线程 ----
    // read_layer1_thread_count = 2：2个"第一层"读取线程，把4个writer均分给它们，每个读取线程负责2个writer, 各自并发地把数据搬到第二层缓存
    constexpr std::size_t writer_count = 4, per_writer = 1000, read_layer1_thread_count = 2;
    auto w0 = make_writer<Writer<>>(WriterSourceConfig{.thread_id = 0});
    auto w1 = make_writer<Writer<>>(WriterSourceConfig{.thread_id = 1});
    auto w2 = make_writer<Writer<>>(WriterSourceConfig{.thread_id = 2});
    auto w3 = make_writer<Writer<>>(WriterSourceConfig{.thread_id = 3});

    // 消费回调按 thread_id 分流，逐路校验"顺序 + 数量"。
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
        {&w0, &w1, &w2, &w3},                                        // 多 writer
        TwoBufQueueOptions{.read_layer1_thread_count = read_layer1_thread_count}); // 多读线程

    //--------------- 2. 每个 writer 由独立线程写 per_writer 条 -----------------
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
    } // 离开作用域 join 全部写线程

    //--------------- 3. 结束 --------------------------------------------------
    queue.stop();  // 等所有记录消费完，停掉全部读线程

    //--------------- 4. 校验：每个 writer 的数量与顺序都正确 -------------------
    bool ok = valid;
    for (std::size_t i = 0; i < writer_count; ++i)
        ok = ok && written[i] && received[i] == per_writer;
    example::require(ok && queue.error_code() == ReadError::none, "multi-thread writer mismatch");
    std::puts("PASS: 4 writer threads, 2 read threads, 4000 records");
} catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
