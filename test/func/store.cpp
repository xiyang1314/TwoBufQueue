#include "support.hpp"
namespace ft {
void store_cases(Cases &cases) {
    // S_copy：append 完整复制源数据，源随后被清零不影响块内副本。
    cases.push_back({"S_copy", [] {
        ReadBlock block(64, 8);
        auto a = framed(17);
        auto saved = a;
        CHECK(block.append(2, a.data(), a.size(), 987));
        std::fill(a.begin(), a.end(), std::byte{0});
        auto batch = to_data_batch(block);
        CHECK(batch.data_size == 6);
        CHECK(batch.frames.size() == 1);
        CHECK(batch.frames[0].thread_id == 2);
        CHECK(batch.frames[0].read_time_ns == 987);
        CHECK(std::memcmp(batch.frames[0].data, saved.data(), 6) == 0);
        // 领取句柄未归还前阻止回收；归还后 all_released 为真。
        ReadBlockPool pool(1, 2, ReadBlockFactory{64, 8});
        OutputQueue q(pool, 2);
        auto r = q.try_acquire();
        CHECK(r);
        CHECK(q.send(r.loan));
        CHECK(!pool.all_released());
        auto loan = q.try_receive();
        CHECK(loan);
        CHECK(!pool.all_released());
        CHECK(q.finish(loan));
        CHECK(pool.all_released());
    }});
    // S_split：超出块剩余字节空间后 append 失败，调用方须换新块。
    cases.push_back({"S_split", [] {
        ReadBlock block(10, 8);
        auto a = framed(17), b = framed(23);
        CHECK(block.append(2, a.data(), a.size(), 123));
        CHECK(!block.append(4, b.data(), b.size(), 123));
        CHECK(block.frames().size() == 1);
        CHECK(block.data_size() == 6);
        CHECK(std::memcmp(block.frames()[0].data, a.data(), 6) == 0);
    }});
    // S_zero：空范围不登记 frame。
    cases.push_back({"S_zero", [] {
        ReadBlock block(64, 8);
        auto a = framed(1);
        CHECK(block.append(0, nullptr, 0, 1));
        CHECK(block.append(1, a.data(), 0, 1));
        CHECK(block.frames().size() == 0);
        CHECK(block.data_size() == 0);
    }});
    // S_queuefull：队列容量背压，取出归还后可再次领取。
    cases.push_back({"S_queuefull", [] {
        ReadBlockPool pool(1, 4, ReadBlockFactory{64, 8});
        OutputQueue q(pool, 1);
        auto r1 = q.try_acquire();
        CHECK(r1);
        auto r2 = q.try_acquire();
        CHECK(!r2);
        CHECK(r2.status == AcquireStatus::QueueFull);
        CHECK(q.send(r1.loan));
        auto loan = q.try_receive();
        CHECK(loan);
        CHECK(q.finish(loan));
        auto r3 = q.try_acquire();
        CHECK(r3);
        CHECK(q.send(r3.loan));
        auto l2 = q.try_receive();
        CHECK(l2);
        CHECK(q.finish(l2));
        CHECK(pool.all_released());
    }});
    // S_poolfull：池上限背压，归还后可再次领取。
    cases.push_back({"S_poolfull", [] {
        ReadBlockPool pool(1, 1, ReadBlockFactory{64, 8});
        OutputQueue q(pool, 4);
        auto r1 = q.try_acquire();
        CHECK(r1);
        auto r2 = q.try_acquire();
        CHECK(!r2);
        CHECK(r2.status == AcquireStatus::PoolLimit);
        CHECK(q.send(r1.loan));
        auto loan = q.try_receive();
        CHECK(loan);
        CHECK(q.finish(loan));
        auto r3 = q.try_acquire();
        CHECK(r3);
        CHECK(q.send(r3.loan));
        auto l2 = q.try_receive();
        CHECK(l2);
        CHECK(q.finish(l2));
        CHECK(pool.all_released());
    }});
    // S_slots：frame 名额耗尽后 append 失败。
    cases.push_back({"S_slots", [] {
        ReadBlock block(100, 1);
        auto a = framed(17), b = framed(23);
        CHECK(block.append(2, a.data(), a.size(), 1));
        CHECK(!block.append(4, b.data(), b.size(), 1));
        CHECK(block.frames().size() == 1);
        CHECK(block.data_size() == 6);
    }});
    // S_closed：关闭后领取失败且原因精确。
    cases.push_back({"S_closed", [] {
        ReadBlockPool pool(1, 2, ReadBlockFactory{64, 8});
        {
            OutputQueue q(pool, 2);
            auto r = q.try_acquire();
            CHECK(r);
            CHECK(q.send(r.loan));
            auto loan = q.try_receive();
            CHECK(loan);
            CHECK(q.finish(loan));
            q.close();
            CHECK(q.is_closed());
            auto r2 = q.try_acquire();
            CHECK(!r2);
            CHECK(r2.status == AcquireStatus::Closed);
            CHECK(pool.all_released());
        }
    }});
    // S_retry：read_records 进度持久化（不重复交付）；格式错误粘滞、重试仍失败。
    cases.push_back({"S_retry", [] {
        ReadBlock block(64, 8);
        auto a = framed(17), b = framed(23);
        CHECK(block.append(2, a.data(), a.size(), 1));
        CHECK(block.append(4, b.data(), b.size(), 1));
        std::size_t seen = 0;
        auto cb = [&](std::int32_t, const std::byte *, std::size_t) noexcept { ++seen; };
        CHECK(read_records<std::uint16_t>(block, cb));
        CHECK(seen == 2);
        CHECK(read_records<std::uint16_t>(block, cb));
        CHECK(seen == 2);
        ReadBlock bad(64, 8);
        std::byte one{0x04};
        CHECK(bad.append(2, &one, 1, 1));
        auto cb2 = [&](std::int32_t, const std::byte *, std::size_t) noexcept { ++seen; };
        CHECK(!read_records<std::uint16_t>(bad, cb2));
        CHECK(bad.record_error() == ReadRecordError::incomplete_length);
        CHECK(!read_records<std::uint16_t>(bad, cb2));
    }});
    // S_notify：池归还回调每次归还触发一次，清除回调后不再触发。
    cases.push_back({"S_notify", [] {
        ReadBlockPool pool(1, 2, ReadBlockFactory{64, 8});
        std::atomic<unsigned> released{0};
        pool.set_release_callback([](void *p) noexcept {
            static_cast<std::atomic<unsigned> *>(p)->fetch_add(1);
        }, &released);
        {
            auto loan = pool.get();
            CHECK(loan);
            CHECK(released.load() == 0);
            CHECK(pool.recycle(loan));
            CHECK(released.load() == 1);
            pool.set_release_callback(nullptr, nullptr);
            auto loan2 = pool.get();
            CHECK(loan2);
            CHECK(pool.recycle(loan2));
            CHECK(released.load() == 1);
        }
        CHECK(pool.all_released());
    }});
    // S_timeout：池耗尽时 wait_get 超时返回空凭证。
    cases.push_back({"S_timeout", [] {
        ReadBlockPool pool(1, 1, ReadBlockFactory{64, 8});
        auto loan = pool.get();
        CHECK(loan);
        CHECK(!pool.wait_get(2ms));
        CHECK(pool.recycle(loan));
        CHECK(pool.all_released());
    }});
    // S_invalid_*：非法来源、负数编号、超块帧、超批次、粘滞无效来源。
    for (std::string k : {"null", "id", "block", "batch", "prevalidate"})
        cases.push_back({"S_invalid_" + k, [k] {
            auto bytes = payload(128);
            if (k == "prevalidate") {
                ReadBlockPool pool(1, 2, ReadBlockFactory{64, 8});
                OutputQueue q(pool, 2);
                ToReadBlock saver(q);
                CHECK(!saver.read_complete(nullptr));
                CHECK(saver.error_code() == ToReadBlockError::invalid_source);
                CHECK(!saver.read_complete(nullptr));
                CHECK(pool.all_released());
                return;
            }
            if (k == "batch") {
                ReadBlock small(4, 8);
                CHECK(!small.append(0, bytes.data(), 5, 1));
                CHECK(small.frames().size() == 0);
                return;
            }
            ReadBlock block(64, 8);
            bool ok = true;
            if (k == "null")
                ok = block.append(0, nullptr, 2, 1);
            else if (k == "id")
                ok = block.append(-1, bytes.data(), 2, 1);
            else if (k == "block")
                ok = block.append(0, bytes.data(), 65, 1);
            CHECK(!ok);
            CHECK(block.frames().size() == 0);
            CHECK(block.data_size() == 0);
        }});
}
} // namespace ft
