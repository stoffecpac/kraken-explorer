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

// pcapng block encoding shared by the trace export and the TraceRecorder.
//
// Expected bytes are assembled by hand from the pcapng specification
// (draft-ietf-opsawg-pcapng: SHB 0x0A0D0D0A, IDB 1, EPB 6, byte-order magic
// 0x1A2B3C4D, options if_name=2 / shb_userappl=4), LINKTYPE_CAN_SOCKETCAN = 227
// (tcpdump.org link-layer types, can_id in network byte order) and
// <linux/can.h> (CAN_EFF_FLAG 0x80000000, CAN_RTR_FLAG 0x40000000,
// CAN_ERR_FLAG 0x20000000, CANFD_BRS 0x01, CAN_MTU 16, CANFD_MTU 72).
// Never from Kraken Explorer output.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include "core/bus_message.h"
#include "core/pcapng.h"
#include "core/socket_can.h"

namespace
{

using Bytes = std::vector<uint8_t>;

// "0A0D 0D0A" -> bytes; spaces are ignored.
Bytes hex(std::string_view spaced)
{
    Bytes out;
    int hi = -1;
    for (const char c : spaced)
    {
        int v = -1;
        if (c >= '0' && c <= '9') { v = c - '0'; }
        else if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; }
        else if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; }
        if (v < 0) { continue; }
        if (hi < 0) { hi = v; }
        else { out.push_back(static_cast<uint8_t>(hi << 4 | v)); hi = -1; }
    }
    return out;
}

BusMessage frame(uint32_t id, bool extended, int64_t timestamp_us, const Bytes& data)
{
    BusMessage m{ .id = id, .ts_ns = timestamp_us * 1000 };
    if (extended) { m.flags |= bus_flag::extended; }
    set_length(m, static_cast<int>(data.size()));
    std::ranges::copy(data, m.data.begin());
    return m;
}

Bytes packet(const BusMessage& m, uint32_t iface)
{
    Bytes out;
    pcapng_append_packet(out, m, iface);
    return out;
}

Bytes mid(const Bytes& b, size_t offset, size_t count)
{
    return Bytes(b.begin() + static_cast<ptrdiff_t>(offset), b.begin() + static_cast<ptrdiff_t>(offset + count));
}

uint32_t le32(const Bytes& b, size_t offset)
{
    return uint32_t{b[offset]} | uint32_t{b[offset + 1]} << 8 | uint32_t{b[offset + 2]} << 16 | uint32_t{b[offset + 3]} << 24;
}

}

TEST_CASE("section header block")
{
    const Bytes expected = hex(
        "0A0D0D0A"            // block type SHB
        "34000000"            // total length 52
        "4D3C2B1A"            // byte-order magic, little-endian
        "0100 0000"           // version 1.0
        "FFFFFFFFFFFFFFFF"    // section length unspecified
        "0400 0F00"           // shb_userappl, 15 bytes
        "4B72616B656E204578706C6F726572 00"  // "Kraken Explorer" + 1 pad byte
        "0000 0000"           // opt_endofopt
        "34000000");          // total length again
    Bytes out;
    pcapng_append_section_header(out);
    CHECK(out == expected);
}

TEST_CASE("interface description block")
{
    const Bytes expected = hex(
        "01000000"            // block type IDB
        "24000000"            // total length 36
        "E300 0000"           // LINKTYPE_CAN_SOCKETCAN (227), reserved
        "48000000"            // snaplen 72 (CANFD_MTU)
        "0200 0500"           // if_name, 5 bytes
        "7663616E30 000000"   // "vcan0" + padding to 4
        "0000 0000"           // opt_endofopt
        "24000000");
    Bytes out;
    pcapng_append_interface(out, "vcan0");
    CHECK(out == expected);
}

TEST_CASE("classic frame enhanced packet block")
{
    // 0x100000002 us: exercises both timestamp halves.
    const BusMessage m = frame(0x123, false, 0x100000002LL, { 0xAA, 0xBB, 0xCC });

    const Bytes expected = hex(
        "06000000"            // block type EPB
        "30000000"            // total length 48
        "01000000"            // interface index 1
        "01000000 02000000"   // timestamp high, low
        "10000000 10000000"   // captured / original length: CAN_MTU
        "00000123"            // can_id, network byte order
        "03 000000"           // can_dlc + padding
        "AABBCC0000000000"    // data[8]
        "30000000");
    CHECK(packet(m, 1) == expected);
}

TEST_CASE("can_id flags")
{
    struct Row { const char* name; uint32_t id; bool extended, rtr, error; const char* can_id; };
    constexpr Row rows[] = {
        { "standard",       0x123,      false, false, false, "00000123" },
        { "extended",       0x18DAF110, true,  false, false, "98DAF110" },
        { "extended + RTR", 0x18DAF110, true,  true,  false, "D8DAF110" },
        // Unclassified error: CAN_ERR_FLAG | CAN_ERR_BUSERROR (<linux/can/error.h>);
        // the message's own identifier plays no part in an error frame.
        { "error frame",    0x040,      false, false, true,  "20000080" },
    };
    for (const Row& r : rows)
    {
        CAPTURE(r.name);
        BusMessage m = frame(r.id, r.extended, 0, {});
        if (r.rtr)   { m.flags |= bus_flag::rtr; }
        if (r.error) { m.errors = bus_error::generic; }
        CHECK(mid(packet(m, 0), 28, 4) == hex(r.can_id));
    }
}

TEST_CASE("CAN FD frame")
{
    Bytes data;
    for (int i = 0; i < 48; ++i)
    {
        data.push_back(static_cast<uint8_t>(i + 1));
    }
    BusMessage m = frame(0x456, false, 0, data);
    m.flags |= bus_flag::fd | bus_flag::brs;

    const Bytes block = packet(m, 0);

    REQUIRE(block.size() == 104);            // 28 header + CANFD_MTU 72 + 4 trailer
    CHECK(le32(block, 4) == 104);
    CHECK(le32(block, 100) == 104);
    CHECK(le32(block, 20) == 72);            // captured length
    CHECK(le32(block, 24) == 72);            // original length
    CHECK(mid(block, 28, 4) == hex("00000456"));
    CHECK(block[32] == 48);                  // len
    CHECK(block[33] == 0x01);                // flags: CANFD_BRS
    CHECK(mid(block, 34, 2) == hex("0000"));
    CHECK(mid(block, 36, 48) == data);
    CHECK(mid(block, 84, 16) == Bytes(16, 0)); // unused payload zeroed
}

TEST_CASE("decode_frame: a remote frame keeps its DLC and no payload (T87b a1 F7)")
{
    // struct can_frame (linux/can.h): can_id 0x124 | CAN_RTR_FLAG, can_dlc 3, pad, data garbage
    const std::uint8_t bytes[16] = {0x24, 0x01, 0x00, 0x40, 3, 0, 0, 0, 0xAA, 0xBB, 0xCC, 0, 0, 0, 0, 0};
    BusMessage m{};
    socket_can::decode_frame(bytes, sizeof(bytes), m);
    CHECK(m.id == 0x124);
    CHECK(has_flag(m, bus_flag::rtr));
    CHECK(m.len == 3);
    CHECK(std::all_of(m.data.begin(), m.data.end(), [](std::uint8_t b) { return b == 0; }));
}
