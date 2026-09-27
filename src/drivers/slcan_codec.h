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

#pragma once

// Pure SLCAN (Lawicel) ASCII frame decoding, kept apart from the serial driver so
// it can be exercised without a port. A line of ASCII goes in, frame fields come
// out. The DLC table is bus_dlc_lengths / bus_length_to_dlc() from bus_message.h.

#include <cstdint>
#include <string_view>

#include "core/bus_message.h"

namespace slcan
{

inline constexpr int std_id_len = 3;
inline constexpr int ext_id_len = 8;

[[nodiscard]] constexpr char hex_nibble(uint8_t v) noexcept
{
    return v < 10 ? static_cast<char>('0' + v) : static_cast<char>('A' + v - 10);
}

[[nodiscard]] constexpr int from_hex_nibble(char c) noexcept
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    return -1;
}

// Decodes one SLCAN frame line (without the trailing '\r') into msg.
//
// Sets id, flags (extended/rtr/fd/brs, no tx), errors (0), len/dlc and data; the
// data area beyond the payload is zeroed. Timestamp, iface and direction are the
// caller's. Returns false and leaves msg unspecified if the line is not a valid
// frame -- including a data line whose payload is shorter than its DLC claims.
// A standard id spelled above 0x7FF is masked to 11 bits.
//
// Remote frames ('r'/'R') carry a DLC but no payload.
[[nodiscard]] constexpr bool parse_frame_line(std::string_view line, BusMessage& msg) noexcept
{
    if (line.empty()) { return false; }

    uint16_t flags = 0;
    switch (line[0])
    {
        case 't':                                                                 break;
        case 'T': flags = bus_flag::extended;                                     break;
        case 'r': flags = bus_flag::rtr;                                          break;
        case 'R': flags = bus_flag::extended | bus_flag::rtr;                     break;
        case 'd': flags = bus_flag::fd;                                           break;
        case 'D': flags = bus_flag::extended | bus_flag::fd;                      break;
        case 'b': flags = bus_flag::fd | bus_flag::brs;                           break;
        case 'B': flags = bus_flag::extended | bus_flag::fd | bus_flag::brs;      break;
        default:
            return false;
    }

    const bool extended = (flags & bus_flag::extended) != 0;
    const bool fd = (flags & bus_flag::fd) != 0;
    const int id_len = extended ? ext_id_len : std_id_len;
    const size_t min_len = 1 + id_len + 1; // type + id + DLC

    if (line.size() < min_len) { return false; }

    uint32_t id = 0;
    for (int i = 1; i <= id_len; ++i)
    {
        const int nibble = from_hex_nibble(line[i]);
        if (nibble < 0) { return false; }
        id = (id << 4) | static_cast<uint32_t>(nibble);
    }

    const int dlc = from_hex_nibble(line[1 + id_len]);
    if (dlc < 0 || (!fd && dlc > 8))
    {
        return false;
    }

    // A remote frame requests data instead of carrying it: its DLC states how
    // many bytes are being asked for, and no payload digits follow it.
    const int data_len = bus_dlc_lengths[dlc];
    const int payload_bytes = (flags & bus_flag::rtr) ? 0 : data_len;

    if (line.size() < min_len + static_cast<size_t>(payload_bytes) * 2) { return false; }

    msg.id = id & (extended ? can_id_mask_extended : can_id_mask_standard);
    msg.flags = flags;
    msg.errors = 0;
    msg.type = BusType::CAN;
    set_length(msg, data_len);
    msg.data = {};

    for (int i = 0; i < payload_bytes; ++i)
    {
        const int hi = from_hex_nibble(line[min_len + 2 * i]);
        const int lo = from_hex_nibble(line[min_len + 2 * i + 1]);
        if (hi < 0 || lo < 0) { return false; }
        msg.data[i] = static_cast<uint8_t>((hi << 4) | lo);
    }

    return true;
}

} // namespace slcan
