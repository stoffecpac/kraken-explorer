#include "core/trace.h"

#include <algorithm>
#include <format>

#include "core/log.h"

void trace_append(Trace& t, std::span<const BusMessage> msgs)
{
    if (!t.file.empty() && !msgs.empty())
    {
        trace_clear(t); // live frames end the file view
    }
    while (!msgs.empty())
    {
        const std::size_t used = (t.end - t.begin) % trace_chunk_size;
        if (used == 0 && trace_size(t) == t.chunks.size() * trace_chunk_size)
        {
            t.chunks.push_back(std::make_unique_for_overwrite<BusMessage[]>(trace_chunk_size));
        }
        const std::size_t n = std::min(msgs.size(), trace_chunk_size - used);
        std::copy_n(msgs.begin(), n, t.chunks.back().get() + used);
        t.end += n;
        msgs = msgs.subspan(n);
    }

    // Drop the oldest whole chunk while at least max_size messages would remain: O(1) per chunk.
    bool pruned = false;
    while (t.chunks.size() > 1 && trace_size(t) - trace_chunk_size >= t.max_size)
    {
        t.chunks.pop_front();
        t.begin += trace_chunk_size;
        pruned = true;
    }
    if (pruned && !t.prune_warned)
    {
        t.prune_warned = true; // once per trace, keeps the log readable
        log_warning(std::format("Trace reached its limit of {} messages, oldest messages are discarded", t.max_size));
    }
}

void trace_clear(Trace& t)
{
    t.chunks.clear();
    t.file = {};
    t.file_overflow = {};
    t.begin = t.end; // indices keep counting, so stale indices never alias new messages
    t.prune_warned = false;
    ++t.clears;
}

void trace_open_file(Trace& t, std::span<const FrameCacheRec> file, std::span<const FrameCachePayload> overflow)
{
    trace_clear(t);
    t.file = file;
    t.file_overflow = overflow;
    t.end = t.begin + file.size();
}

void rx_deliver(Inbox& inbox, std::span<const RxConsumer> consumers, std::span<const BusMessage> msgs,
                void (*wake)())
{
    for (const auto& m : msgs)
    {
        for (const auto& c : consumers)
        {
            c.fn(c.user, m);
        }
    }
    {
        std::scoped_lock lock(inbox.mutex);
        inbox.msgs.insert(inbox.msgs.end(), msgs.begin(), msgs.end());
    }
    if (wake)
    {
        wake();
    }
}

void inbox_take(Inbox& inbox, std::vector<BusMessage>& batch)
{
    std::scoped_lock lock(inbox.mutex);
    batch.insert(batch.end(), inbox.msgs.begin(), inbox.msgs.end());
    inbox.msgs.clear();
}

void trace_append_sorted(Trace& t, std::vector<BusMessage>& batch)
{
    // The batch is one already-sorted run per interface: merging the runs is O(n) where a full
    // stable sort of ~10k frames per frame cost the main thread ~20 % under a flood.
    // Stable merges give exactly the stable_sort result; many runs (jittery timestamps) fall back.
    const auto proj = &BusMessage::ts_ns;
    auto mid = std::ranges::is_sorted_until(batch, {}, proj);
    for (int runs = 0; mid != batch.end() && runs < 8; ++runs)
    {
        const auto last = std::ranges::is_sorted_until(mid, batch.end(), {}, proj);
        std::ranges::inplace_merge(batch.begin(), mid, last, {}, proj);
        mid = last;
    }
    if (mid != batch.end())
    {
        std::ranges::stable_sort(batch, {}, proj); // stable: each interface keeps its order
    }
    trace_append(t, batch);
    batch.clear();
}
