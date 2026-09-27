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

// Frame-level BusMessage behaviour: payload length and DLC, identifier/flag
// separation and the display formatters. These are the invariants every driver
// relies on when it builds a frame, so a change here breaks all of them at once.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string>

#include "core/bus_message.h"

TEST_CASE("default constructed frame is an empty classic CAN RX frame")
{
    const BusMessage m;

    CHECK(m.id == 0);
    CHECK(m.flags == 0);
    CHECK(m.errors == 0);
    CHECK(m.len == 0);
    CHECK(m.dlc == 0);
    CHECK(m.type == BusType::CAN);
    CHECK(!is_error_frame(m));
    CHECK(!has_flag(m, bus_flag::tx));
}

// set_length stores a byte count, not a DLC code: 12 means 12 bytes (DLC 9).
TEST_CASE("set_length stores the length verbatim and keeps dlc in sync")
{
    // Classic CAN, then the CAN FD steps, then the boundary.
    // ISO 11898-1:2015 Table 5 DLC for each length (a partial FD step rounds up).
    constexpr struct { int len; uint8_t dlc; } rows[] = {
        { 0, 0 }, { 1, 1 }, { 8, 8 }, { 12, 9 }, { 16, 10 }, { 20, 11 },
        { 24, 12 }, { 32, 13 }, { 48, 14 }, { 49, 15 }, { 63, 15 }, { 64, 15 },
    };
    for (const auto& r : rows)
    {
        CAPTURE(r.len);
        BusMessage m;
        set_length(m, r.len);
        CHECK(m.len == r.len);
        CHECK(m.dlc == r.dlc);
    }
}

// Documents a deliberately surprising fallback: a length outside 0..64 is
// coerced to 8, not clamped to 64 and not rejected.
TEST_CASE("out-of-range length falls back to eight")
{
    for (const int length : { -1, 65, 100, 255 })
    {
        CAPTURE(length);
        BusMessage m;
        set_length(m, length);
        CHECK(m.len == 8);
        CHECK(m.dlc == 8);
    }
}

// The DLC code table and bus_length_to_dlc must be inverses for the valid FD
// lengths; otherwise a frame sent out comes back with a different length.
TEST_CASE("dlc table and bus_length_to_dlc round trip")
{
    constexpr std::array<uint8_t, 16> iso_lengths = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };
    CHECK(bus_dlc_lengths == iso_lengths);

    for (uint8_t dlc = 0; dlc < 16; ++dlc)
    {
        CAPTURE(dlc);
        CHECK(bus_length_to_dlc(iso_lengths[dlc]) == dlc);
    }
}

TEST_CASE("can_id masks to 11 or 29 bits by the extended flag")
{
    CHECK(can_id(BusMessage{ .id = 0x123 }) == 0x123);
    CHECK(can_id(BusMessage{ .id = 0x1FFFFFFF, .flags = bus_flag::extended }) == 0x1FFFFFFF);
    CHECK(can_id(BusMessage{ .id = 0xFFFFFFFF, .flags = bus_flag::extended }) == 0x1FFFFFFF);

    // The stored id is untouched; only the view is masked.
    BusMessage m{ .id = 0x1ABCDEF, .flags = bus_flag::extended | bus_flag::rtr };
    m.flags &= ~bus_flag::extended;
    CHECK(can_id(m) == 0x5EF);
    m.flags |= bus_flag::extended;
    CHECK(can_id(m) == 0x1ABCDEF);
    CHECK(has_flag(m, bus_flag::rtr));
}

// Unlike the pre-port setId(), a large id does NOT imply an extended frame: the
// parser or driver sets bus_flag::extended explicitly (DBC: bit 31 of the raw id).
TEST_CASE("large id does not imply extended")
{
    const BusMessage m{ .id = 0x800 };
    CHECK(!has_flag(m, bus_flag::extended));
    CHECK(can_id(m) == 0x000);
}

TEST_CASE("error frame is any errors bit, independent of flags")
{
    BusMessage m{ .id = 0x1FFFFFFF, .flags = bus_flag::fd | bus_flag::brs };
    CHECK(!is_error_frame(m));

    m.errors = bus_error::crc;
    CHECK(is_error_frame(m));
    CHECK(has_flag(m, bus_flag::fd));
    CHECK(has_flag(m, bus_flag::brs));
    CHECK(m.id == 0x1FFFFFFF);

    m.errors = 0;
    CHECK(!is_error_frame(m));
}

TEST_CASE("append_id")
{
    std::string s;
    append_id(s, BusMessage{ .id = 0x12 });
    CHECK(s == "0x012");

    s.clear();
    append_id(s, BusMessage{ .id = 0x1ABCDEF0, .flags = bus_flag::extended });
    CHECK(s == "0x1ABCDEF0");

    s.clear();
    append_id(s, BusMessage{ .id = 0x3C, .type = BusType::LIN });
    CHECK(s == "0x3C");

    // Appends rather than overwrites, so a reused buffer can build a row.
    append_id(s, BusMessage{ .id = 0x7FF });
    CHECK(s == "0x3C0x7FF");
}

// Settings "Data display: Hex / ASCII": non-printable bytes render as '.', one
// token per byte (space-separated) so per-byte colouring still works.
TEST_CASE("append_bytes hex and ascii")
{
    constexpr uint8_t bytes[] = { 'A', 0x00, '!', 0x7F, 0x01, 0xFF };

    std::string s;
    append_bytes(s, bytes, true);
    CHECK(s == "A . ! . . . ");

    s.clear();
    append_bytes(s, bytes, false);
    CHECK(s == "41 00 21 7F 01 FF");

    s.clear();
    append_bytes(s, {}, false);
    append_bytes(s, {}, true);
    CHECK(s.empty());
}

TEST_CASE("append_error_flags")
{
    std::string s;
    append_error_flags(s, bus_error::ack | bus_error::crc);
    CHECK(s == "ERROR: ACK+CRC");

    s.clear();
    append_error_flags(s, 0);
    CHECK(s == "ERROR");
}

TEST_CASE("append_data picks payload, error flags or nothing")
{
    BusMessage m{ .id = 0x100 };
    std::string s;
    append_data(s, m, false);
    CHECK(s.empty());

    set_length(m, 3);
    m.data[0] = 0x01;
    m.data[1] = 'B';
    m.data[2] = 0xFF;
    append_data(s, m, false);
    CHECK(s == "01 42 FF");

    s.clear();
    append_data(s, m, true);
    CHECK(s == ". B . ");

    s.clear();
    m.errors = bus_error::bus_off;
    append_data(s, m, false);
    CHECK(s == "ERROR: BUSOFF");
}
