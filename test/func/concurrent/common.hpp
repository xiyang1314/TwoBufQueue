#pragma once
#include "../support.hpp"
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>

namespace ct {
using namespace ft;
using Bytes = std::vector<std::byte>;
using Clock = std::chrono::steady_clock;

struct Failure {
    std::atomic<bool> failed{false};
    std::mutex mutex;
    std::string message;
    void save(const std::string &value) {
        std::lock_guard lock(mutex);
        if (!failed.load()) {
            message = value;
            failed.store(true);
        }
    }
    template <class F> void run(F &&f) noexcept {
        try {
            f();
        } catch (const std::exception &e) {
            save(e.what());
        } catch (...) {
            save("non-standard exception in worker");
        }
    }
    void check() {
        if (failed.load())
            throw std::runtime_error(message);
    }
};

inline void put32(Bytes &b, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        b[offset + i] = std::byte((value >> (i * 8)) & 255);
}
inline std::uint32_t get32(const std::byte *b) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= std::to_integer<std::uint32_t>(b[i]) << (i * 8);
    return value;
}
inline Bytes message(unsigned writer, unsigned seq, std::size_t size) {
    CHECK(size >= 16);
    Bytes bytes(size);
    std::uint64_t state = 0x9e3779b97f4a7c15ULL ^ (std::uint64_t(writer + 1) << 32) ^ seq;
    for (auto &byte : bytes) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        byte = std::byte((state * 0x2545f4914f6cdd1dULL) >> 56);
    }
    put32(bytes, 0, writer);
    put32(bytes, 4, seq);
    put32(bytes, 8, static_cast<unsigned>(size));
    put32(bytes, 12, ~seq);
    return bytes;
}
inline void equal_bytes(const std::byte *actual, std::size_t size, const Bytes &expected,
                        const std::string &context) {
    if (size != expected.size())
        throw std::runtime_error(context + " length mismatch");
    for (std::size_t i = 0; i < size; ++i)
        if (actual[i] != expected[i])
            throw std::runtime_error(
                context + " byte=" + std::to_string(i) +
                " expected=" + std::to_string(std::to_integer<unsigned>(expected[i])) +
                " actual=" + std::to_string(std::to_integer<unsigned>(actual[i])));
}
inline void wait_flag(const std::atomic<bool> &flag, Failure &failure, Clock::time_point deadline) {
    while (!flag.load() && !failure.failed.load()) {
        if (Clock::now() > deadline)
            throw std::runtime_error("handshake deadline exceeded");
        std::this_thread::yield();
    }
}

// Synchronize only ownership handoff. Payload validation happens outside this lock.
template <class T> class Channel {
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<T> values_;
    std::size_t capacity_;
    bool closed_ = false;

  public:
    explicit Channel(std::size_t capacity) : capacity_(capacity) {}
    bool push(T &value) {
        std::lock_guard lock(mutex_);
        if (closed_ || values_.size() == capacity_)
            return false;
        values_.push_back(std::move(value));
        ready_.notify_one();
        return true;
    }
    std::optional<T> pop() {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] { return closed_ || !values_.empty(); });
        if (values_.empty())
            return {};
        T result = std::move(values_.back()); // Deliberately release out of FIFO order.
        values_.pop_back();
        return result;
    }
    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        ready_.notify_all();
    }
};

void pipeline_cases(Cases &);
void pool_cases(Cases &);
void block_alloc_cases(Cases &);
void block_pool_cases(Cases &);
void queue_cases(Cases &);
} // namespace ct
