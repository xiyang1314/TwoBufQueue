#include "../support/read_test_support.hpp"
using namespace read_test;
using Point = ReadItem::TimePoint;
static void empty_backoff() {
    auto cfg = config(); cfg.idle_rescan_interval = 5ms; cfg.idle_rescan_max_interval = 30ms;
    ReadItem item(cfg);
    auto now = Point{1s};
    for (auto interval : {5ms, 10ms, 20ms, 30ms, 30ms}) {
        CHECK(!item.should_read_current(now, false, 0));
        CHECK(item.next_read_time() == now + interval);
        now = item.next_read_time();
    }
}
static void early_scan() {
    ReadItem item(config());
    const auto now = Point{1s};
    CHECK(!item.should_read_current(now, false, 0));
    const auto deadline = item.next_read_time();
    for (int i = 0; i != 100; ++i) {
        CHECK(!item.should_read_current(now, false, 0));
        CHECK(!item.should_read_current(now, false, 6));
        CHECK(item.next_read_time() == deadline);
    }
    CHECK(!item.should_read_current(deadline, false, 0));
    CHECK(item.next_read_time() == deadline + 2ms);
}
static void data_resets() {
    ReadItem item(config()); auto now = Point{1s};
    for (int i = 0; i != 5; ++i) {
        CHECK(!item.should_read_current(now, false, 0)); now = item.next_read_time();
    }
    CHECK(item.should_read_current(now, false, 6));
    CHECK(item.next_read_time() == now + 1ms);
    now = item.next_read_time();
    CHECK(!item.should_read_current(now, false, 0));
    CHECK(item.next_read_time() == now + 1ms);
    now = item.next_read_time();
    CHECK(!item.should_read_current(now, false, 0));
    CHECK(item.next_read_time() == now + 2ms);
}
static void force_and_independent() {
    ReadItem a(config()), b(config()); const auto now = Point{1s};
    CHECK(!a.should_read_current(now, false, 0));
    CHECK(!a.should_read_current(now, true, 0));
    CHECK(a.next_read_time() == now + 2ms);
    CHECK(a.should_read_current(now, true, 6));
    CHECK(a.next_read_time() == now + 1ms);
    CHECK(!b.should_read_current(now, false, 0));
    CHECK(b.next_read_time() == now + 1ms);
}
static void saturation() {
    auto cfg = config(); cfg.idle_rescan_interval = 10ns; cfg.idle_rescan_max_interval = 35ns;
    ReadItem item(cfg); auto now = Point{1s};
    for (auto interval : {10ns, 20ns, 35ns, 35ns}) {
        CHECK(!item.should_read_current(now, false, 0));
        CHECK(item.next_read_time() == now + interval); now = item.next_read_time();
    }
    CHECK(!item.should_read_current(Point::max() - 5ns, true, 0));
    CHECK(item.next_read_time() == Point::max());
    cfg.idle_rescan_interval = std::chrono::nanoseconds::max() / 2 + 1ns;
    cfg.idle_rescan_max_interval = std::chrono::nanoseconds::max();
    ReadItem large(cfg);
    CHECK(!large.should_read_current(Point{}, true, 0));
    CHECK(!large.should_read_current(Point{}, true, 0));
    CHECK(large.next_read_time() == Point::max());
    CHECK(large.should_read_current(Point::min(), true, 6));
    CHECK(large.next_read_time() == Point::min() + cfg.idle_rescan_interval);
}
int main(int argc, char** argv) {
    return run_cases(argc, argv, {{"empty_backoff", empty_backoff}, {"early_scan", early_scan},
        {"data_resets", data_resets}, {"force_and_independent", force_and_independent},
        {"saturation", saturation}});
}
