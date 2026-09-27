#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "core/trace.h"

static std::vector<BusMessage> numbered(uint64_t first, std::size_t n)
{
    std::vector<BusMessage> v(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        v[i].ts_ns = static_cast<int64_t>(first + i);
    }
    return v;
}

TEST_CASE("append across chunk boundaries keeps global indices")
{
    Trace t{.max_size = 1000000};
    const auto a = numbered(0, trace_chunk_size - 3);
    const auto b = numbered(a.size(), trace_chunk_size + 10);
    trace_append(t, a);
    trace_append(t, b);
    CHECK(t.chunks.size() == 3);
    CHECK(trace_size(t) == a.size() + b.size());
    for (uint64_t i : {uint64_t{0}, uint64_t{trace_chunk_size - 1}, uint64_t{trace_chunk_size}, t.end - 1})
    {
        CAPTURE(i);
        CHECK(trace_at(t, i).ts_ns == static_cast<int64_t>(i));
    }
}

TEST_CASE("max_size drops whole chunks, indices stay stable")
{
    Trace t{.max_size = 100};
    trace_append(t, numbered(0, 2 * trace_chunk_size + 5));
    // Two full chunks dropped, the last partial one (5 < 100) is kept too: 65541 left.
    CHECK(t.chunks.size() == 2);
    CHECK(t.begin == trace_chunk_size);
    CHECK(trace_size(t) >= t.max_size);
    CHECK(trace_at(t, t.begin).ts_ns == static_cast<int64_t>(trace_chunk_size));
    CHECK(trace_at(t, t.end - 1).ts_ns == static_cast<int64_t>(t.end - 1));
    CHECK(t.prune_warned);

    trace_clear(t);
    CHECK(trace_size(t) == 0);
    trace_append(t, numbered(t.end, 1));
    CHECK(trace_at(t, t.begin).ts_ns == static_cast<int64_t>(t.begin));
}

TEST_CASE("rx_deliver runs consumers, wakes and drains into the trace")
{
    static std::atomic<int> wakes = 0;
    int consumed = 0;
    const RxConsumer consumer{.fn = [](void* user, const BusMessage&) { ++*static_cast<int*>(user); },
                              .user = &consumed};
    Inbox inbox;
    Trace trace;
    std::vector<BusMessage> scratch;
    constexpr int per_thread = 20000;
    std::thread rx([&]
    {
        for (int i = 0; i < per_thread; i += 100)
        {
            rx_deliver(inbox, std::span(&consumer, 1), numbered(i, 100), [] { ++wakes; });
        }
    });
    while (trace.end < per_thread)
    {
        inbox_take(inbox, scratch);
        trace_append_sorted(trace, scratch);
    }
    rx.join();
    inbox_take(inbox, scratch);
    trace_append_sorted(trace, scratch);
    CHECK(trace.end == per_thread);
    CHECK(consumed == per_thread);
    CHECK(wakes == per_thread / 100);
    CHECK(trace_at(trace, per_thread - 1).ts_ns == per_thread - 1);
}

TEST_CASE("two interfaces with overlapping times drain into a time-ordered trace")
{
    Inbox a;
    Inbox b;
    const auto frame = [](uint16_t iface, int64_t ts) { return BusMessage{.iface = iface, .ts_ns = ts}; };
    a.msgs = {frame(0, 10), frame(0, 30), frame(0, 30), frame(0, 50)};
    b.msgs = {frame(1, 20), frame(1, 30), frame(1, 40)};
    Trace trace;
    std::vector<BusMessage> batch;
    inbox_take(a, batch);
    inbox_take(b, batch);
    trace_append_sorted(trace, batch);

    CHECK(batch.empty());
    CHECK(a.msgs.empty());
    CHECK(b.msgs.empty());
    const std::vector<std::pair<int64_t, uint16_t>> expected = {
        {10, 0}, {20, 1}, {30, 0}, {30, 0}, {30, 1}, {40, 1}, {50, 0}}; // stable on ties
    REQUIRE(trace_size(trace) == expected.size());
    for (uint64_t i = 0; i < expected.size(); ++i)
    {
        CAPTURE(i);
        CHECK(trace_at(trace, i).ts_ns == expected[i].first);
        CHECK(trace_at(trace, i).iface == expected[i].second);
    }
}

// T87b a1 F6: a frame of vcan1 lands one main-loop frame after a newer one of vcan0; the trace
// keeps arrival order, the copy that files and scripts get is in time order.
TEST_CASE("trace_copy is time-ordered across main-loop batches")
{
    const auto frame = [](uint16_t iface, int64_t ts) { return BusMessage{.iface = iface, .ts_ns = ts}; };
    Trace trace;
    std::vector<BusMessage> batch = {frame(0, 10), frame(0, 30)};
    trace_append_sorted(trace, batch);
    batch = {frame(1, 20), frame(0, 40)};
    trace_append_sorted(trace, batch);

    CHECK(trace_at(trace, 2).ts_ns == 20); // arrival order in the trace
    const std::vector<BusMessage> copy = trace_copy(trace);
    const std::vector<std::pair<int64_t, uint16_t>> expected = {{10, 0}, {20, 1}, {30, 0}, {40, 0}};
    REQUIRE(copy.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        CAPTURE(i);
        CHECK(copy[i].ts_ns == expected[i].first);
        CHECK(copy[i].iface == expected[i].second);
    }
}
