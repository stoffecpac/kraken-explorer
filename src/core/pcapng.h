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

// pcapng (IETF draft-ietf-opsawg-pcapng) block encoding for LINKTYPE_CAN_SOCKETCAN.
// Shared by the one-shot export and the streaming recorder.
//
// A file is one Section Header Block followed by Interface Description Blocks
// and Enhanced Packet Blocks. An IDB may appear anywhere in the section as long
// as it precedes the first EPB that references its index, which lets a recorder
// add interfaces as they show up. All fields are little-endian.
//
// Every function appends to a caller-owned byte buffer.

#include <bit>
#include <concepts>
#include <cstdint>
#include <string_view>
#include <vector>

#include "core/bus_message.h"

// Appends `value` little-endian, whatever the host byte order. Also used by the
// pcap and MDF4 writers.
template <typename T>
    requires std::integral<T> || std::floating_point<T>
void append_le(std::vector<uint8_t>& out, T value)
{
    using U = std::make_unsigned_t<std::conditional_t<std::floating_point<T>,
                                                      std::conditional_t<sizeof(T) == 8, int64_t, int32_t>, T>>;
    const auto bits = std::bit_cast<U>(value);
    for (size_t i = 0; i < sizeof(U); ++i)
    {
        out.push_back(static_cast<uint8_t>(bits >> (8 * i)));
    }
}

// Section Header Block with unspecified section length, so blocks can be
// appended without rewriting the header.
void pcapng_append_section_header(std::vector<uint8_t>& out);

// Interface Description Block (LINKTYPE_CAN_SOCKETCAN, if_name option).
void pcapng_append_interface(std::vector<uint8_t>& out, std::string_view name);

// `m` as a SocketCAN can_frame (16 bytes) or canfd_frame (72 bytes), the packet
// data of LINKTYPE_CAN_SOCKETCAN in both pcap and pcapng.
void append_socketcan_frame(std::vector<uint8_t>& out, const BusMessage& m);

// Enhanced Packet Block carrying `m` as a SocketCAN frame, microsecond timestamp.
// `iface_index` is the 0-based position of the interface's IDB in the section.
void pcapng_append_packet(std::vector<uint8_t>& out, const BusMessage& m, uint32_t iface_index);
