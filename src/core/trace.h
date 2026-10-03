#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "core/bus_message.h"
#include "core/frame_cache_rec.h"

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
    // File view (trace_open_file): every frame of a loaded file as its frame cache's 32-byte
    // records, mmap'ed, decoded on read in place of the chunks. Indices [begin, end) map to
    // file[index - begin]. Appending leaves the file view.
    std::span<const FrameCacheRec> file;
    std::span<const FrameCachePayload> file_overflow; // payloads of the file's frames with len > 8
};

[[nodiscard]] inline uint64_t trace_size(const Trace& t) noexcept { return t.end - t.begin; }

// begin <= index < end. By value: a file view decodes its record (a live frame is a copy, which
// is nothing next to what callers do per frame).
[[nodiscard]] inline BusMessage trace_at(const Trace& t, uint64_t index) noexcept
{
    const uint64_t rel = index - t.begin;
    if (!t.file.empty())
    {
        return frame_cache_decode(t.file[rel], t.file_overflow);
    }
    return t.chunks[rel / trace_chunk_size][rel % trace_chunk_size];
}

// Every retained message, ordered by ts_ns (stable). The trace itself is in arrival order: a
// frame of one interface can land a main-loop frame after a newer one of another (see
// trace_append_sorted), and files / scripts want time order.
[[nodiscard]] inline std::vector<BusMessage> trace_copy(const Trace& t)
{
    std::vector<BusMessage> out;
    out.reserve(trace_size(t));
    for (uint64_t i = t.begin; i < t.end; ++i)
    {
        out.push_back(trace_at(t, i));
    }
    std::ranges::stable_sort(out, {}, &BusMessage::ts_ns);
    return out;
}

void trace_append(Trace& t, std::span<const BusMessage> msgs);
void trace_clear(Trace& t);
// Clears the trace and shows `file` instead (sorted by ts_ns, with its overflow payloads, both
// kept alive by the caller until the next trace_clear / trace_open_file / trace_append). Views
// that consume new frames as they arrive (graph, instrument panel, ...) skip a file view: it is
// not live traffic.
void trace_open_file(Trace& t, std::span<const FrameCacheRec> file, std::span<const FrameCachePayload> overflow);

// Called on an RX thread for every frame before it is queued, e.g. the Python
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
// per frame only: a frame arriving a frame late is not moved back (trace_copy sorts). batch is reused between
// frames so the steady state does not allocate.
void inbox_take(Inbox& inbox, std::vector<BusMessage>& batch); // appends, keeps inbox capacity
void trace_append_sorted(Trace& t, std::vector<BusMessage>& batch); // stable sort, append, clear
