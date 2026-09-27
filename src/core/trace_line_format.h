/*
  Copyright (c) 2026 Schildkroet

  This file is part of cangaroo.

  cangaroo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  cangaroo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with cangaroo.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

// Line-oriented text trace formats. Shared by the one-shot export and the
// streaming recorder so both produce identical output. Every append_* function
// appends to a caller-owned buffer, without a trailing newline unless stated.

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "core/bus_message.h"

inline constexpr std::string_view asc_footer = "End TriggerBlock";

// Vector ASC header block, `start` in local time; every line, including the last, ends in '\n'.
void append_asc_header(std::string& out, std::chrono::system_clock::time_point start);

// One ASC event line. `start_ns` is the timestamp relative times are measured from.
void append_asc_line(std::string& out, const BusMessage& m, int64_t start_ns, int channel);

// One Linux candump -L line.
void append_candump_line(std::string& out, const BusMessage& m, std::string_view iface_name);

// Parses a whitespace-split ASC CAN FD event ("<time> CANFD <channel> ...") into
// `m`: id, flags, direction, channel as iface, length and data. The timestamp is
// left to the caller. Accepts the Vector layout, with or without a symbolic frame
// name, and the layout cangaroo wrote before it was fixed:
//   Vector:   <id> [name] <BRS> <ESI> <DLC hex> <data length> <data...>
//   legacy:   <id> <flags> 0 0 <length> <length> <data...>
// Returns false for anything malformed.
[[nodiscard]] bool parse_asc_canfd_line(std::span<const std::string_view> parts, BusMessage& m);

// PEAK PCAN trace (.trc). Written as version 2.1 with the python-can column set
// (;$COLUMNS=N,O,T,B,I,d,R,L,D), CRLF line ends as PEAK writes them. Read: 1.0,
// 1.1, 1.3 (fixed columns) and 2.0 / 2.1 (driven by ;$COLUMNS, with the PEAK
// defaults when absent). ;$STARTTIME is days since 1899-12-30 and is taken as
// UTC, as python-can does.
// ponytail: PEAK tools write STARTTIME in local time; absolute times of their files
// are off by the UTC offset (relative times are exact). Upgrade: a time-zone option.
struct TrcLayout
{
    std::string columns = "NOILD";  // one letter per column ('D' = rest of the line); 1.0 until a header says otherwise
    int64_t start_ns = 0;           // ;$STARTTIME as ns since the Unix epoch
};

// Header line (";$FILEVERSION=", ";$STARTTIME=", ";$COLUMNS="); anything else is ignored.
void parse_trc_header_line(std::string_view line, TrcLayout& layout);

// One whitespace-split frame line into `m`: timestamp (start + offset), id, flags,
// direction, bus number (1 when the layout has none) as iface, length, data.
// DT/FD/FB/FE/BI/RR/ER in 2.x, Rx/Tx/Error in 1.x; false for status/event lines
// and anything malformed.
[[nodiscard]] bool parse_trc_line(std::span<const std::string_view> parts, const TrcLayout& layout, BusMessage& m);

// Version 2.1 header; every line ends in "\r\n".
void append_trc_header(std::string& out, int64_t start_ns);

// One version 2.1 line: message `number` (1-based), time relative to `start_ns`, 1-based `bus`.
void append_trc_line(std::string& out, const BusMessage& m, uint64_t number, int64_t start_ns, int bus);
