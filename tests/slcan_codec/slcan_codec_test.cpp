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

// SLCAN (Lawicel) ASCII frame decoding.
//
// This is attacker-adjacent code in the sense that matters for a bus tool: the
// bytes come from a device over a serial link and may be truncated, mistimed or
// garbage. The malformed-input cases below are the point of the file -- a short
// line must be rejected, never read past its end.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include "core/bus_message.h"
#include "drivers/slcan_codec.h"

using slcan::parse_frame_line;

TEST_CASE("standard data frame")
{
    BusMessage m;
    REQUIRE(parse_frame_line("t1234DEADBEEF", m));

    CHECK(m.id == 0x123);
    CHECK(m.flags == 0);
    CHECK(m.errors == 0);
    CHECK(m.len == 4);
    CHECK(m.data[0] == 0xDE);
    CHECK(m.data[1] == 0xAD);
    CHECK(m.data[2] == 0xBE);
    CHECK(m.data[3] == 0xEF);
}

TEST_CASE("extended data frame")
{
    BusMessage m;
    REQUIRE(parse_frame_line("T1FFFFFFF80011223344556677", m));

    CHECK(m.id == 0x1FFFFFFF);
    CHECK(m.flags == bus_flag::extended);
    CHECK(m.len == 8);
    CHECK(m.data[0] == 0x00);
    CHECK(m.data[7] == 0x77);
}

TEST_CASE("remote frames with DLC 0")
{
    BusMessage zero;
    REQUIRE(parse_frame_line("r1230", zero));
    CHECK(zero.id == 0x123);
    CHECK(zero.flags == bus_flag::rtr);
    CHECK(zero.len == 0);

    BusMessage ext;
    REQUIRE(parse_frame_line("R000004000", ext));
    CHECK(ext.id == 0x400);
    CHECK(ext.flags == (bus_flag::rtr | bus_flag::extended));
    CHECK(ext.len == 0);
}

// A remote frame requests data rather than carrying it, so "r1238" is a complete
// SLCAN line: "remote request for 8 bytes from id 0x123". The DLC must be kept as
// the requested length while no payload digits are expected. Earlier revisions
// demanded 16 payload hex digits here and dropped every such frame as an RX error.
TEST_CASE("remote frame with non-zero DLC is accepted")
{
    BusMessage m;
    REQUIRE(parse_frame_line("r1238", m));
    CHECK(m.id == 0x123);
    CHECK(m.flags == bus_flag::rtr);
    CHECK(m.len == 8);

    BusMessage ext;
    REQUIRE(parse_frame_line("R1FFFFFFF4", ext));
    CHECK(ext.id == 0x1FFFFFFF);
    CHECK(ext.flags == (bus_flag::rtr | bus_flag::extended));
    CHECK(ext.len == 4);
}

// The requested length is reported, but the data area must not carry anything --
// including leftovers from a previous frame decoded into the same BusMessage.
TEST_CASE("remote frame data area is zeroed")
{
    BusMessage m;
    REQUIRE(parse_frame_line("t1238DEADBEEFCAFEF00D", m));
    CHECK(m.data[0] == 0xDE);

    REQUIRE(parse_frame_line("r1238", m));
    CHECK(m.len == 8);
    CHECK(m.data == decltype(m.data){});
}

// Trailing payload on a remote frame is surplus, not a reason to reject the line.
TEST_CASE("remote frame ignores trailing payload")
{
    BusMessage m;
    REQUIRE(parse_frame_line("r1232AABB", m));
    CHECK(has_flag(m, bus_flag::rtr));
    CHECK(m.len == 2);
    CHECK(m.data[0] == 0);
    CHECK(m.data[1] == 0);
}

// The caller owns timestamp, interface and direction; decoding a frame from an
// error frame's leftovers must still clear the error bits and old flags.
TEST_CASE("caller fields are kept, frame fields are replaced")
{
    BusMessage m{ .flags = bus_flag::tx | bus_flag::fd, .errors = bus_error::crc, .iface = 3, .ts_ns = 42 };
    REQUIRE(parse_frame_line("t1230", m));
    CHECK(m.id == 0x123);
    CHECK(m.len == 0);
    CHECK(m.dlc == 0);
    CHECK(m.flags == 0);
    CHECK(m.errors == 0);
    CHECK(m.iface == 3);
    CHECK(m.ts_ns == 42);
}

TEST_CASE("lowercase hex")
{
    BusMessage m;
    REQUIRE(parse_frame_line("t7ab2dead", m));
    CHECK(m.id == 0x7AB);
    CHECK(m.len == 2);
    CHECK(m.data[0] == 0xDE);
    CHECK(m.data[1] == 0xAD);
}

// A standard-format line can spell an id above the 11-bit range. The frame is
// accepted as standard and the id is masked to 11 bits. Documented rather than
// endorsed: such a line is malformed SLCAN to begin with.
TEST_CASE("standard id above eleven bits is masked")
{
    BusMessage m;
    REQUIRE(parse_frame_line("tabc2dead", m));
    CHECK(!has_flag(m, bus_flag::extended));
    CHECK(m.id == (0xABCu & 0x7FFu));
}

TEST_CASE("FD frame")
{
    // DLC nibble 9 means 12 bytes on an FD frame.
    BusMessage m;
    REQUIRE(parse_frame_line("d1009000102030405060708090A0B", m));

    CHECK(m.id == 0x100);
    CHECK(m.flags == bus_flag::fd);
    CHECK(m.len == 12);
    CHECK(m.dlc == 9);
    CHECK(m.data[0] == 0x00);
    CHECK(m.data[11] == 0x0B);
}

TEST_CASE("FD frame with bitrate switch")
{
    BusMessage m;
    REQUIRE(parse_frame_line("b2001AA", m));

    CHECK(m.id == 0x200);
    CHECK(m.flags == (bus_flag::fd | bus_flag::brs));
    CHECK(m.len == 1);
    CHECK(m.data[0] == 0xAA);

    BusMessage ext;
    REQUIRE(parse_frame_line("B1234567800", ext));
    CHECK(ext.flags == (bus_flag::extended | bus_flag::fd | bus_flag::brs));
    CHECK(ext.id == 0x12345678);
}

// The CAN FD DLC nibble is a code, not a byte count: 9..F mean 12..64 bytes.
TEST_CASE("FD DLC nibble mapping")
{
    constexpr struct { char nibble; int bytes; } rows[] = {
        { '0', 0 }, { '8', 8 }, { '9', 12 }, { 'A', 16 }, { 'B', 20 },
        { 'C', 24 }, { 'D', 32 }, { 'E', 48 }, { 'F', 64 },
    };
    for (const auto& r : rows)
    {
        CAPTURE(r.nibble);
        const std::string line = std::string("d100") + r.nibble + std::string(r.bytes * 2, '5');   // 0x55 payload

        BusMessage m;
        REQUIRE(parse_frame_line(line, m));
        CHECK(m.len == r.bytes);
        if (r.bytes > 0)
        {
            CHECK(m.data[r.bytes - 1] == 0x55);
        }
    }
}

TEST_CASE("rejects empty line")
{
    BusMessage m;
    CHECK(!parse_frame_line("", m));
}

TEST_CASE("rejects unknown type character")
{
    BusMessage m;
    for (const char* line : { "x1238", "z1238", "S6", "F", "1238" })
    {
        CAPTURE(line);
        CHECK(!parse_frame_line(line, m));
    }
}

TEST_CASE("rejects short line")
{
    BusMessage m;
    // Missing DLC, missing id digits, id shorter than an extended id needs.
    for (const char* line : { "t", "t1", "t12", "t123", "T1FFFFFF", "D1234567", "r12" })
    {
        CAPTURE(line);
        CHECK(!parse_frame_line(line, m));
    }
}

// The important one: a data frame's DLC promises more payload than the line
// carries. (Remote frames are exempt -- they legitimately carry none.)
TEST_CASE("rejects truncated payload")
{
    BusMessage m;
    CHECK(!parse_frame_line("t1238DEADBEEF", m));        // claims 8, has 4
    CHECK(!parse_frame_line("t1234DEADBE", m));          // claims 4, has 3.5
    CHECK(!parse_frame_line("d100F0011", m));            // claims 64, has 2
    CHECK(!parse_frame_line("T1FFFFFFF8AABB", m));       // extended, claims 8
}

TEST_CASE("rejects non-hex identifier")
{
    BusMessage m;
    CHECK(!parse_frame_line("tG238DEADBEEF", m));
    CHECK(!parse_frame_line("t1-38DEADBEEF", m));
    CHECK(!parse_frame_line("t12 4DEADBEEF", m));
}

TEST_CASE("rejects non-hex payload")
{
    BusMessage m;
    CHECK(!parse_frame_line("t1232DEZZ", m));
    CHECK(!parse_frame_line("t1231G0", m));
}

// A classic CAN frame cannot carry more than 8 bytes, so DLC 9..F is invalid
// there even though it is meaningful for FD.
TEST_CASE("rejects classic DLC above eight")
{
    BusMessage m;
    for (const char* line : { "t1239", "t123A", "t123F", "r1239" })
    {
        CAPTURE(line);
        CHECK(!parse_frame_line(line, m));
    }
}

TEST_CASE("rejects FD DLC that is not a hex digit")
{
    BusMessage m;
    CHECK(!parse_frame_line("d100G", m));
}

// The line is a view into a receive buffer: bytes after it must never be read.
TEST_CASE("does not read past the view")
{
    constexpr char buffer[] = "t1238DEADBEEFCAFEF00D";
    BusMessage m;
    CHECK(!parse_frame_line(std::string_view(buffer, 12), m));   // cut inside the payload
    CHECK(parse_frame_line(std::string_view(buffer, 21), m));
}

TEST_CASE("hex nibble encoding")
{
    CHECK(slcan::hex_nibble(0) == '0');
    CHECK(slcan::hex_nibble(9) == '9');
    CHECK(slcan::hex_nibble(10) == 'A');
    CHECK(slcan::hex_nibble(15) == 'F');

    // hex_nibble and from_hex_nibble must round trip.
    for (uint8_t v = 0; v < 16; ++v)
    {
        CAPTURE(v);
        CHECK(slcan::from_hex_nibble(slcan::hex_nibble(v)) == v);
    }

    CHECK(slcan::from_hex_nibble('g') == -1);
    CHECK(slcan::from_hex_nibble(' ') == -1);
    CHECK(slcan::from_hex_nibble('\0') == -1);
}
