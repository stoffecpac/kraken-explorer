#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "core/bus_message.h"

// Chunked append-only store of every received/sent frame. Main thread only.
// Indices are global and stable: message i stays at index i until its chunk is pruned,
// so filters and selections can keep plain indices. Valid range is [begin, end).
inline constexpr std::size_t trace_chunk_size = 65536;

struct Trace
{
    std::deque<std::unique_ptr<BusMessage[]>> chunks;
    uint64_t begin = 0;         // global index of the oldest retained message
    uint64_t end = 0;           // global index one past the newest message (= total appended)
    uint64_t max_size = 500000; // trace/maxSize, shared by all interfaces: whole chunks are dropped while >= max_size remain
    bool prune_warned = false;
    uint64_t clears = 0;        // bumped by trace_clear, so views can tell a clear from pruning
};

[[nodiscard]] inline uint64_t trace_size(const Trace& t) noexcept { return t.end - t.begin; }

// begin <= index < end.
[[nodiscard]] inline const BusMessage& trace_at(const Trace& t, uint64_t index) noexcept
{
    const uint64_t rel = index - t.begin;
    return t.chunks[rel / trace_chunk_size][rel % trace_chunk_size];
}

// Every retained message, oldest first.
[[nodiscard]] inline std::vector<BusMessage> trace_copy(const Trace& t)
{
    std::vector<BusMessage> out;
    out.reserve(trace_size(t));
    for (uint64_t i = t.begin; i < t.end; ++i)
    {
        out.push_back(trace_at(t, i));
    }
    return out;
}

void trace_append(Trace& t, std::span<const BusMessage> msgs);
void trace_clear(Trace& t);

// Called on an RX thread for every frame before it is queued, e.g. Gateway, the Python
// RX hook, the recorder queue (the old Qt::DirectConnection consumers). Must be cheap and
// thread-safe. The consumer list is only changed while no RX thread runs.
struct RxConsumer
{
    void (*fn)(void* user, const BusMessage& msg) = nullptr;
    void* user = nullptr;
};

// Per-interface hand-off from its RX thread to the main thread.
struct Inbox
{
    std::mutex mutex;
    std::vector<BusMessage> msgs;
};

// RX thread: runs the direct consumers, queues msgs and wakes the main loop (may be null).
void rx_deliver(Inbox& inbox, std::span<const RxConsumer> consumers, std::span<const BusMessage> msgs,
                void (*wake)());

// Main thread, once per frame: inbox_take every inbox into one batch, then
// trace_append_sorted, so frames of different interfaces interleave by ts_ns. Ordering is
// per frame only: a frame arriving a frame late is not moved back. batch is reused between
// frames so the steady state does not allocate.
void inbox_take(Inbox& inbox, std::vector<BusMessage>& batch); // appends, keeps inbox capacity
void trace_append_sorted(Trace& t, std::vector<BusMessage>& batch); // stable sort, append, clear
