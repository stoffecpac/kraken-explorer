/*
  Copyright (c) 2026 Schildkroet

  This file is part of Kraken Explorer.

  Kraken Explorer is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  Kraken Explorer is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Kraken Explorer.  If not, see <http://www.gnu.org/licenses/>.
*/

// Frame cache: a trace file parsed once into a binary file that is mmap'ed on every later
// open, so an 8 GB log opens instantly and every jump is a binary search. Lives next to the
// replay parsers (ui/replay.cpp) because it reuses them.
//
// Layout (all little-endian, native structs): FrameCacheHeader at 0, then from frames_off
// FrameCacheRec[frame_count] sorted by ts_ns (core/frame_cache_rec.h: 32 bytes, data inline up to
// 8 bytes), FrameCachePayload[overflow_count] (the longer payloads, indexed from their records),
// FrameCacheRow[row_count] sorted by (channel, id), uint32 row_frames (per row, its frame indices
// in time order), channel names ('\n' separated).

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "core/bus_message.h"
#include "core/frame_cache_rec.h"
#include "core/trace_file_format.h"
#include "ui/replay.h"

struct CanDbMessage;
struct Setup;

inline constexpr uint32_t frame_cache_version = 7; // 7: 1 MB hash blocks

struct FrameCacheHeader
{
    char magic[8] = {'K', 'R', 'K', 'F', 'C', 'A', 'C', 'H'};
    uint32_t version = frame_cache_version;
    uint32_t msg_size = sizeof(FrameCacheRec); // the cache is invalid when the record changes
    uint64_t src_size = 0;
    int64_t src_mtime = 0;                  // ns, file_time_type epoch
    uint64_t src_hash = 0;                  // of the whole source: a touched but unchanged file keeps its cache
    uint64_t frame_count = 0;
    uint64_t row_count = 0;
    uint64_t overflow_count = 0;
    uint64_t frames_off = 0;
    uint64_t overflow_off = 0;
    uint64_t rows_off = 0;
    uint64_t row_frames_off = 0;
    uint64_t channels_off = 0;
    uint64_t channels_size = 0;
};

// One (channel, id) of the file, as ReplayIdRow; its frames are row_frames[first, first + count).
struct FrameCacheRow
{
    uint32_t id = 0;        // replay_error_id for error frames
    uint16_t channel = 0;
    uint8_t extended = 0;   // of the first frame with this id
    uint8_t dirs = 0;       // bit 0: has rx, bit 1: has tx
    uint64_t first = 0;
    uint64_t count = 0;
    // Cycle times between consecutive frames of the row (count - 1 of them), for the Monitor's
    // Cycle columns without a pass over the file.
    int64_t cycle_min_ns = INT64_MAX;
    int64_t cycle_max_ns = 0;
    int64_t cycle_sum_ns = 0;
};

// An open cache. Plain views into one read-only mapping; frame_cache_close unmaps.
struct FrameCache
{
    void* map = nullptr;
    std::size_t map_size = 0;
    std::span<const FrameCacheRec> recs;        // sorted by ts_ns; frame_cache_frame decodes one
    std::span<const FrameCachePayload> overflow; // payloads of the recs with len > 8
    std::span<const FrameCacheRow> rows;
    std::span<const uint32_t> row_frames; // ponytail: uint32 frame indices, max 4G frames
    std::vector<std::string> channels;
};

[[nodiscard]] inline BusMessage frame_cache_frame(const FrameCache& c, uint64_t i) noexcept
{
    return frame_cache_decode(c.recs[i], c.overflow);
}

// $XDG_CACHE_HOME/kraken-explorer/<hash of the path>.kfc (~/.cache without XDG).
[[nodiscard]] std::filesystem::path frame_cache_path(const std::filesystem::path& src);

// Maps `cache` if it was built from `src` as it is now. A new mtime with the same size hashes src:
// unchanged content keeps the cache (its mtime is updated). Errors: "No cache", "Old cache"
// (another format version), "Stale cache" (src's content changed), others.
[[nodiscard]] std::expected<FrameCache, std::string> frame_cache_open(const std::filesystem::path& src,
                                                                      const std::filesystem::path& cache);

// Parses src (mmap'ed, in blocks of whole lines for text formats) into a new cache and returns it
// mapped. With RAM to spare the cache is built in memory (memfd) and returned at once, and a
// background thread saves it to `cache` (no waiting for a slow disk); otherwise it is written to
// a temporary file renamed at the end. A cancelled or failed build leaves no cache behind.
[[nodiscard]] std::expected<FrameCache, std::string> frame_cache_build(const std::filesystem::path& src,
                                                                       const std::filesystem::path& cache, TraceFileFormat format,
                                                                       const ReplayParseProgress& progress = {});

// Waits until every background save of frame_cache_build is on disk (tests; the app at exit).
void frame_cache_wait_saved();

// Removes the least recently opened .kfc files of `dir` (never `keep`, the one just built) until
// the .kfc total is <= max_bytes, and .tmp leftovers of crashed builds older than an hour.
// frame_cache_build calls it with frame_cache_max_bytes.
inline constexpr uint64_t frame_cache_max_bytes = uint64_t{32} << 30; // ponytail: fixed cap, a setting could expose it
void frame_cache_prune(const std::filesystem::path& dir, uint64_t max_bytes, const std::filesystem::path& keep);

void frame_cache_close(FrameCache& c);


// The frame indices (time order) of every cache row whose frames decode to `msg` in `setup`:
// one row's list copied, several rows' lists merged. Empty when none.
[[nodiscard]] std::vector<uint32_t> frame_cache_message_frames(const FrameCache& c, const Setup& setup, const CanDbMessage* msg);

