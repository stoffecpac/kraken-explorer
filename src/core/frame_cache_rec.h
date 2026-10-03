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

// One frame of a frame cache (ui/frame_cache.cpp) as stored on disk and mmap'ed: 32 bytes instead
// of BusMessage's 88, so an 8 GB candump makes a ~5 GB cache instead of 16 and the build writes
// a third of the bytes. Up to 8 data bytes (classic CAN, LIN: nearly every frame) are inline; a
// longer payload (CAN FD) lives in the cache's overflow area of 64-byte payloads and `data`
// holds its index. Decoded into a BusMessage on every read (one memcpy). In core because the
// Trace's file view (core/trace.h) reads these records in place.

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#include "core/bus_message.h"

struct FrameCacheRec
{
    int64_t ts_ns = 0;
    uint32_t id = 0;
    uint16_t flags = 0;
    uint16_t errors = 0;
    uint16_t iface = 0;
    uint8_t dlc = 0;
    uint8_t len = 0;                // > 8: data[0..4) is the uint32 index of the payload in the overflow area
    BusType type = BusType::CAN;
    uint8_t reserved[3] = {};
    uint8_t data[8] = {};
};
static_assert(sizeof(FrameCacheRec) == 32 && std::is_trivially_copyable_v<FrameCacheRec>);

using FrameCachePayload = std::array<uint8_t, bus_max_data_bytes>;

// The caller stores m.data at overflow[overflow_index] when m.len > 8 (ignored otherwise).
[[nodiscard]] inline FrameCacheRec frame_cache_encode(const BusMessage& m, uint32_t overflow_index) noexcept
{
    FrameCacheRec r{.ts_ns = m.ts_ns, .id = m.id, .flags = m.flags, .errors = m.errors, .iface = m.iface,
                    .dlc = m.dlc, .len = m.len, .type = m.type};
    if (m.len > 8)
    {
        std::memcpy(r.data, &overflow_index, sizeof overflow_index);
    }
    else
    {
        std::memcpy(r.data, m.data.data(), sizeof r.data);
    }
    return r;
}

[[nodiscard]] inline BusMessage frame_cache_decode(const FrameCacheRec& r, std::span<const FrameCachePayload> overflow) noexcept
{
    BusMessage m{.id = r.id, .flags = r.flags, .errors = r.errors, .iface = r.iface, .dlc = r.dlc, .len = r.len,
                 .type = r.type, .ts_ns = r.ts_ns};
    if (r.len > 8)
    {
        uint32_t index = 0;
        std::memcpy(&index, r.data, sizeof index);
        m.data = overflow[index];
    }
    else
    {
        std::memcpy(m.data.data(), r.data, sizeof r.data);
    }
    return m;
}
