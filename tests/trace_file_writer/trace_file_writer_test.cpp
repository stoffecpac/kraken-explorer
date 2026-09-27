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

// Whole trace files as written by the export (write_trace_file).
//
// Expected structure comes from the format definitions and the input frames:
//   candump -L       "(<sec>.<usec>) <iface> <id>#<data>"
//   Vector ASC       header block, relative times, 1-based channels, footer
//   libpcap          0xA1B2C3D4 global header v2.4, 16-byte record headers
//   pcapng           SHB, IDBs before use, EPB interface indices
//   ASAM MDF 4.1     block ids and sizes (24-byte header + 8 per link + data),
//                    link targets, one time master, non-overlapping channels
//   <linux/can.h>    can_frame / canfd_frame layout, flag bits
// Line and block encoders are covered by trace_line_format and pcapng_format;
// tests/format_compat additionally reads these files with python-can, scapy
// and asammdf.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/bus_message.h"
#include "core/trace_file_writer.h"

namespace
{

using Bytes = std::vector<uint8_t>;

constexpr int64_t base_us = 1757000000000000LL;

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

uint16_t le16(const Bytes& b, uint64_t o) { return static_cast<uint16_t>(b[o] | b[o + 1] << 8); }
uint32_t le32(const Bytes& b, uint64_t o) { return uint32_t{le16(b, o)} | uint32_t{le16(b, o + 2)} << 16; }
uint64_t le64(const Bytes& b, uint64_t o) { return uint64_t{le32(b, o)} | uint64_t{le32(b, o + 4)} << 32; }

double le_double(const Bytes& b, uint64_t o)
{
    const uint64_t bits = le64(b, o);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

std::string str(const Bytes& b, uint64_t o, uint64_t n)
{
    return std::string(b.begin() + static_cast<ptrdiff_t>(o), b.begin() + static_cast<ptrdiff_t>(o + n));
}

std::string iface_name(uint16_t id)
{
    if (id == 7) { return "vcan0"; }
    if (id == 3) { return "vcan1"; }
    return "if" + std::to_string(id);
}

BusMessage frame(int64_t timestamp_us, uint16_t iface, uint32_t id, bool extended, bool rx, const Bytes& data)
{
    BusMessage m{ .id = id, .iface = iface, .ts_ns = timestamp_us * 1000 };
    if (extended) { m.flags |= bus_flag::extended; }
    if (!rx)      { m.flags |= bus_flag::tx; }
    set_length(m, static_cast<int>(data.size()));
    std::ranges::copy(data, m.data.begin());
    return m;
}

Bytes written(TraceFileFormat format, const std::vector<BusMessage>& messages)
{
    std::ostringstream out(std::ios::binary);
    write_trace_file(out, format, messages, iface_name);
    const std::string s = out.str();
    return Bytes(s.begin(), s.end());
}

std::vector<std::string> lines(const Bytes& content)
{
    std::vector<std::string> out;
    std::string cur;
    for (const uint8_t c : content)
    {
        if (c == '\n')
        {
            if (!cur.empty()) { out.push_back(cur); }
            cur.clear();
        }
        else
        {
            cur += static_cast<char>(c);
        }
    }
    if (!cur.empty()) { out.push_back(cur); }
    return out;
}

// First `count` whitespace-separated tokens.
std::vector<std::string> tokens(const std::string& line, size_t count)
{
    std::istringstream in(line);
    std::vector<std::string> out;
    for (std::string t; out.size() < count && in >> t;)
    {
        out.push_back(t);
    }
    return out;
}

std::string trimmed(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

// --- MDF4 ---

struct MdfBlock
{
    std::string id;
    uint64_t offset = 0;
    uint64_t length = 0;
    uint64_t link_count = 0;
};

MdfBlock mdf_block(const Bytes& file, uint64_t offset)
{
    MdfBlock block{ .offset = offset };
    if (offset != 0 && offset + 24 <= file.size())
    {
        block.id = str(file, offset, 4);
        block.length = le64(file, offset + 8);
        block.link_count = le64(file, offset + 16);
    }
    return block;
}

uint64_t mdf_link(const Bytes& file, const MdfBlock& block, uint64_t index)
{
    return le64(file, block.offset + 24 + 8 * index);
}

uint64_t mdf_data(const MdfBlock& block)
{
    return block.offset + 24 + 8 * block.link_count;
}

std::string mdf_text(const Bytes& file, uint64_t offset)
{
    const MdfBlock block = mdf_block(file, offset);
    const std::string body = str(file, offset + 24, block.length - 24);
    return body.substr(0, body.find('\0'));
}

// Every link of `block` is null or points at an 8-byte aligned block inside the file.
void verify_mdf_links(const Bytes& file, const MdfBlock& block)
{
    for (uint64_t i = 0; i < block.link_count; ++i)
    {
        const uint64_t target = mdf_link(file, block, i);
        if (target == 0)
        {
            continue;
        }
        CAPTURE(block.id);
        CAPTURE(i);
        REQUIRE(target % 8 == 0);
        REQUIRE(target + 24 <= file.size());
        REQUIRE(str(file, target, 2) == "##");
    }
    CAPTURE(block.id);
    REQUIRE(block.length % 8 == 0);
}

struct MdfChannel
{
    std::string name;
    uint8_t type = 0;
    uint8_t sync_type = 0;
    uint8_t data_type = 0;
    uint32_t byte_offset = 0;
    uint32_t bit_count = 0;
};

void verify_mdf4(const Bytes& file, const std::vector<BusMessage>& messages)
{
    // IDBLOCK
    REQUIRE(file.size() >= 64);
    CHECK(str(file, 0, 8) == "MDF     ");
    CHECK(str(file, 8, 8) == "4.10    ");
    CHECK(le16(file, 28) == 410);   // id_ver
    CHECK(le16(file, 60) == 0);     // id_unfin_flags: finalized

    // HDBLOCK: 6 links + 32 bytes of data
    const MdfBlock hd = mdf_block(file, 64);
    REQUIRE(hd.id == "##HD");
    CHECK(hd.link_count == 6);
    CHECK(hd.length == 24 + 6 * 8 + 32);
    verify_mdf_links(file, hd);
    if (!messages.empty())
    {
        CHECK(le64(file, mdf_data(hd)) == static_cast<uint64_t>(messages.front().ts_ns / 1000) * 1000);
    }
    CHECK(file[mdf_data(hd) + 12] == 0);  // time flags: UTC

    // FHBLOCK with an XML <FHcomment> in an MDBLOCK
    const MdfBlock fh = mdf_block(file, mdf_link(file, hd, 1));
    REQUIRE(fh.id == "##FH");
    CHECK(fh.link_count == 2);
    CHECK(fh.length == 24 + 2 * 8 + 16);
    verify_mdf_links(file, fh);
    const MdfBlock fh_comment = mdf_block(file, mdf_link(file, fh, 1));
    REQUIRE(fh_comment.id == "##MD");
    const std::string xml = mdf_text(file, fh_comment.offset);
    CHECK(xml.starts_with("<FHcomment"));
    CHECK(xml.find("<tool_id>") != std::string::npos);
    CHECK(xml.find("<tool_vendor>") != std::string::npos);
    CHECK(xml.find("<tool_version>") != std::string::npos);

    // DGBLOCK: 4 links + 8 bytes
    const MdfBlock dg = mdf_block(file, mdf_link(file, hd, 0));
    REQUIRE(dg.id == "##DG");
    CHECK(dg.link_count == 4);
    CHECK(dg.length == 24 + 4 * 8 + 8);
    verify_mdf_links(file, dg);
    CHECK(file[mdf_data(dg)] == 0);  // rec_id_size

    // CGBLOCK: 6 links + 32 bytes
    const MdfBlock cg = mdf_block(file, mdf_link(file, dg, 1));
    REQUIRE(cg.id == "##CG");
    CHECK(cg.link_count == 6);
    CHECK(cg.length == 24 + 6 * 8 + 32);
    verify_mdf_links(file, cg);
    const uint64_t cg_data = mdf_data(cg);
    CHECK(le64(file, cg_data + 8) == messages.size());  // cg_cycle_count
    const uint32_t record_size = le32(file, cg_data + 24);  // cg_data_bytes
    CHECK(le32(file, cg_data + 28) == 0);                   // cg_inval_bytes
    CHECK(mdf_text(file, mdf_link(file, cg, 2)) == "CAN");

    // CNBLOCKs: 8 links + 72 bytes each
    std::map<std::string, MdfChannel> channels;
    int masters = 0;
    for (uint64_t cn_offset = mdf_link(file, cg, 1); cn_offset != 0;)
    {
        const MdfBlock cn = mdf_block(file, cn_offset);
        REQUIRE(cn.id == "##CN");
        CHECK(cn.link_count == 8);
        CHECK(cn.length == 24 + 8 * 8 + 72);
        verify_mdf_links(file, cn);

        const uint64_t d = mdf_data(cn);
        MdfChannel ch{
            .name = mdf_text(file, mdf_link(file, cn, 2)),
            .type = file[d],
            .sync_type = file[d + 1],
            .data_type = file[d + 2],
            .byte_offset = le32(file, d + 4),
            .bit_count = le32(file, d + 8),
        };
        CAPTURE(ch.name);
        CHECK(file[d + 3] == 0);  // bit offset
        CHECK(uint64_t{ch.byte_offset} * 8 + ch.bit_count <= uint64_t{record_size} * 8);

        if (ch.type == 2)  // master channel
        {
            ++masters;
            CHECK(ch.sync_type == 1);    // time
            CHECK(ch.data_type == 4);    // REAL little-endian
            CHECK(ch.bit_count == 64);
            CHECK(mdf_text(file, mdf_link(file, cn, 6)) == "s");
        }
        channels[ch.name] = ch;
        cn_offset = mdf_link(file, cn, 0);
    }
    CHECK(masters == 1);
    std::vector<std::string> names;
    for (const auto& [name, ch] : channels) { names.push_back(name); }
    REQUIRE(names == std::vector<std::string>{ "CAN_ID", "DLC", "DataBytes", "Dir", "t" });
    CHECK(channels["DataBytes"].data_type == 10);   // byte array
    CHECK(channels["DataBytes"].bit_count == 512);  // room for a full CAN FD payload

    std::vector<MdfChannel> by_offset;
    for (const auto& [name, ch] : channels) { by_offset.push_back(ch); }
    std::ranges::sort(by_offset, {}, &MdfChannel::byte_offset);
    for (size_t i = 1; i < by_offset.size(); ++i)
    {
        const MdfChannel& prev = by_offset[i - 1];
        CAPTURE(prev.name);
        CHECK(uint64_t{prev.byte_offset} * 8 + prev.bit_count <= uint64_t{by_offset[i].byte_offset} * 8);
    }

    // DTBLOCK and records
    const MdfBlock dt = mdf_block(file, mdf_link(file, dg, 2));
    REQUIRE(dt.id == "##DT");
    CHECK(dt.length == 24 + messages.size() * record_size);
    REQUIRE(dt.offset + dt.length <= file.size());

    for (size_t i = 0; i < messages.size(); ++i)
    {
        CAPTURE(i);
        const BusMessage& m = messages[i];
        const uint64_t record = dt.offset + 24 + i * record_size;
        const double expected_time = static_cast<double>(m.ts_ns - messages.front().ts_ns) / 1e9;

        uint32_t can_id = m.id;
        if (has_flag(m, bus_flag::extended)) { can_id |= 0x80000000u; }  // CAN_EFF_FLAG
        if (has_flag(m, bus_flag::rtr))      { can_id |= 0x40000000u; }  // CAN_RTR_FLAG
        if (is_error_frame(m))               { can_id |= 0x20000000u; }  // CAN_ERR_FLAG

        CHECK(std::abs(le_double(file, record + channels["t"].byte_offset) - expected_time) < 1e-9);
        CHECK(le32(file, record + channels["CAN_ID"].byte_offset) == can_id);
        CHECK(file[record + channels["DLC"].byte_offset] == m.len);
        CHECK(file[record + channels["Dir"].byte_offset] == (has_flag(m, bus_flag::tx) ? 1 : 0));
        const uint64_t data = record + channels["DataBytes"].byte_offset;
        CHECK(std::equal(m.data.begin(), m.data.begin() + m.len, file.begin() + static_cast<ptrdiff_t>(data)));
    }
}

}

TEST_CASE("candump file")
{
    const std::vector<BusMessage> messages = {
        frame(base_us, 7, 0x123, false, true, hex("112233")),
        frame(base_us + 1500, 3, 0x456, false, false, hex("AA")),
    };
    CHECK(lines(written(TraceFileFormat::CanDump, messages))
          == std::vector<std::string>{ "(1757000000.000000) vcan0 123#112233", "(1757000000.001500) vcan1 456#AA" });
}

TEST_CASE("ASC file")
{
    const std::vector<BusMessage> messages = {
        frame(base_us, 7, 0x123, false, true, hex("112233")),
        frame(base_us + 10000, 3, 0x456, false, false, hex("AA")),
        frame(base_us + 25000, 7, 0x124, false, true, hex("BB")),
    };
    const auto content = lines(written(TraceFileFormat::VectorAsc, messages));

    REQUIRE(content.size() == 6 + 3 + 1);
    CHECK(content[0].starts_with("date "));
    CHECK(content[1] == "base hex  timestamps absolute");
    CHECK(content[4].starts_with("Begin Triggerblock "));
    CHECK(trimmed(content[5]) == "0.000000 Start of measurement");
    CHECK(content.back() == "End TriggerBlock");

    // Times relative to the first frame; channels numbered by first appearance.
    using V = std::vector<std::string>;
    CHECK(tokens(content[6], 4) == V{ "0.000000", "1", "123", "Rx" });
    CHECK(tokens(content[7], 4) == V{ "0.010000", "2", "456", "Tx" });
    CHECK(tokens(content[8], 4) == V{ "0.025000", "1", "124", "Rx" });
}

TEST_CASE("ASC empty trace is valid")
{
    const auto content = lines(written(TraceFileFormat::VectorAsc, {}));
    REQUIRE(content.size() == 7);
    CHECK(content[0].starts_with("date "));
    CHECK(content.back() == "End TriggerBlock");
}

TEST_CASE("pcap file")
{
    const Bytes fd_data = hex("000102030405060708090A0B");
    BusMessage fd = frame(2500000, 7, 0x18DAF110, true, true, fd_data);
    fd.flags |= bus_flag::fd | bus_flag::brs;
    const std::vector<BusMessage> messages = { frame(1000002, 7, 0x123, false, true, hex("AABBCC")), fd };

    Bytes expected = hex(
        "D4C3B2A1 0200 0400"                  // magic (usec timestamps), version 2.4
        "00000000 00000000"                   // thiszone, sigfigs
        "48000000 E3000000"                   // snaplen 72, LINKTYPE_CAN_SOCKETCAN
        "01000000 02000000 10000000 10000000" // 1 s, 2 us, incl_len = orig_len = CAN_MTU
        "00000123 03 00 00 00"                // can_id, len, __pad, __res0, len8_dlc
        "AABBCC0000000000"
        "02000000 20A10700 48000000 48000000" // 2 s, 500000 us, CANFD_MTU
        "98DAF110 0C 01 00 00");              // CAN_EFF_FLAG | id, len 12, CANFD_BRS
    expected.insert(expected.end(), fd_data.begin(), fd_data.end());
    expected.insert(expected.end(), 64 - fd_data.size(), 0);

    CHECK(written(TraceFileFormat::Pcap, messages) == expected);
}

TEST_CASE("pcap empty trace")
{
    CHECK(written(TraceFileFormat::Pcap, {}) == hex("D4C3B2A1 0200 0400 00000000 00000000 48000000 E3000000"));
}

TEST_CASE("pcapng file")
{
    const std::vector<BusMessage> messages = {
        frame(base_us, 7, 0x123, false, true, hex("11")),
        frame(base_us + 1, 3, 0x124, false, true, hex("22")),
        frame(base_us + 2, 7, 0x125, false, true, hex("33")),
    };
    const Bytes file = written(TraceFileFormat::PcapNg, messages);

    struct Block { uint32_t type; Bytes bytes; };
    std::vector<Block> blocks;
    for (size_t pos = 0; pos < file.size();)
    {
        REQUIRE(pos + 12 <= file.size());
        const uint32_t length = le32(file, pos + 4);
        REQUIRE((length >= 12 && length % 4 == 0 && pos + length <= file.size()));
        CHECK(le32(file, pos + length - 4) == length);
        blocks.push_back({ le32(file, pos), Bytes(file.begin() + static_cast<ptrdiff_t>(pos),
                                                  file.begin() + static_cast<ptrdiff_t>(pos + length)) });
        pos += length;
    }

    REQUIRE(blocks.size() == 1 + 2 + 3);
    CHECK(blocks[0].type == 0x0A0D0D0A);

    // Interfaces described up front in order of first appearance, name in if_name.
    auto if_name = [](const Bytes& idb)
    {
        return le16(idb, 16) == 2 ? str(idb, 20, le16(idb, 18)) : std::string();
    };
    CHECK(blocks[1].type == 1);
    CHECK(le16(blocks[1].bytes, 8) == 227);
    CHECK(if_name(blocks[1].bytes) == "vcan0");
    CHECK(blocks[2].type == 1);
    CHECK(if_name(blocks[2].bytes) == "vcan1");

    constexpr uint32_t expected_interface[] = { 0, 1, 0 };
    for (size_t i = 0; i < 3; ++i)
    {
        CAPTURE(i);
        const Bytes& epb = blocks[3 + i].bytes;
        CHECK(blocks[3 + i].type == 6);
        CHECK(le32(epb, 8) == expected_interface[i]);
        const uint64_t timestamp = (uint64_t{le32(epb, 12)} << 32) | le32(epb, 16);
        CHECK(timestamp == static_cast<uint64_t>(messages[i].ts_ns / 1000));
    }
}

TEST_CASE("pcapng empty trace")
{
    const Bytes file = written(TraceFileFormat::PcapNg, {});
    REQUIRE(file.size() >= 28);
    CHECK(le32(file, 0) == 0x0A0D0D0A);
    CHECK(le32(file, 4) == file.size());  // nothing but the section header
}

TEST_CASE("MDF4 file")
{
    BusMessage fd = frame(base_us + 20500, 7, 0x456, false, true, Bytes(64, 0x5A));
    fd.flags |= bus_flag::fd | bus_flag::brs;
    const std::vector<BusMessage> messages = {
        frame(base_us, 7, 0x123, false, true, hex("112233")),
        frame(base_us + 10000, 3, 0x18DAF110, true, false, hex("0102030405060708")),
        fd,
    };
    verify_mdf4(written(TraceFileFormat::VectorMdf, messages), messages);
}

TEST_CASE("MDF4 empty trace is valid")
{
    verify_mdf4(written(TraceFileFormat::VectorMdf, {}), {});
}

// Two interfaces drained one after another: the trace is not time-ordered.
TEST_CASE("ASC export of unsorted frames has no negative times")
{
    const std::vector<BusMessage> messages = {
        frame(base_us + 20000, 7, 0x123, false, true, hex("11")),
        frame(base_us, 3, 0x456, false, false, hex("22")),
        frame(base_us + 10000, 7, 0x124, false, true, hex("33")),
    };
    const auto content = lines(written(TraceFileFormat::VectorAsc, messages));
    REQUIRE(content.size() == 6 + 3 + 1);
    // t0 = the earliest frame (0x456), not the first one in the span.
    using V = std::vector<std::string>;
    CHECK(tokens(content[6], 3) == V{ "0.020000", "1", "123" });
    CHECK(tokens(content[7], 3) == V{ "0.000000", "2", "456" });
    CHECK(tokens(content[8], 3) == V{ "0.010000", "1", "124" });
}

TEST_CASE("MDF4 export of unsorted frames has a non-decreasing time channel")
{
    const std::vector<BusMessage> messages = {
        frame(base_us + 20000, 7, 0x123, false, true, hex("11")),
        frame(base_us, 3, 0x456, false, false, hex("22")),
        frame(base_us + 10000, 7, 0x124, false, true, hex("33")),
        frame(base_us, 7, 0x125, false, true, hex("44")),
    };
    // Records come out in time order, stable on ties (0x456 before 0x125).
    const std::vector<BusMessage> sorted = { messages[1], messages[3], messages[2], messages[0] };
    verify_mdf4(written(TraceFileFormat::VectorMdf, messages), sorted);
}

TEST_CASE("PEAK TRC 2.1 file")
{
    // Header keys and DT lines as python-can 4.6.1's can.TRCWriter writes them for these
    // frames (PEAK numbers messages from 1, python-can from 0). FD / RTR / ER lines follow
    // the PEAK TRC spec (python-can cannot write them); format_compat reads them back.
    std::vector<BusMessage> messages = {
        frame(base_us, 7, 0x123, false, true, hex("112233")),
        frame(base_us + 10500, 3, 0x18DAF110, true, false, hex("AABBCC")),
        frame(base_us + 20000, 7, 0x456, false, true, hex("00 01 02 03 04 05 06 07 08 09 0A 0B")),
        frame(base_us + 30000, 3, 0x7DF, false, true, hex("00000000")),
        frame(base_us + 40000, 7, 0, false, true, {}),
        frame(base_us + 50000, 7, 0x3C, false, true, hex("01")),
    };
    messages[2].flags |= bus_flag::fd | bus_flag::brs;
    messages[3].flags |= bus_flag::rtr;
    messages[4].errors = bus_error::generic;
    messages[5].type = BusType::LIN;  // no TRC representation: left out

    const auto content = lines(written(TraceFileFormat::Trc, messages));
    REQUIRE(content.size() == 12 + 5);
    CHECK(content[0] == ";$FILEVERSION=2.1\r");
    CHECK(content[1] == ";$STARTTIME=45904.648148148146\r");
    CHECK(content[2] == ";$COLUMNS=N,O,T,B,I,d,R,L,D\r");
    CHECK(std::all_of(content.begin(), content.begin() + 12, [](const std::string& l) { return l.starts_with(';'); }));
    CHECK(content[12] == "      1         0.000 DT  1     0123 Rx -  3    11 22 33\r");
    CHECK(content[13] == "      2        10.500 DT  2 18DAF110 Tx -  3    AA BB CC\r");
    CHECK(content[14] == "      3        20.000 FB  1     0456 Rx -  9    00 01 02 03 04 05 06 07 08 09 0A 0B\r");
    CHECK(content[15] == "      4        30.000 RR  2     07DF Rx -  4\r");
    CHECK(content[16] == "      5        40.000 ER  1        - Rx -  5    08 01 00 00 00\r");
}

TEST_CASE("PEAK TRC empty trace has a header")
{
    const auto content = lines(written(TraceFileFormat::Trc, {}));
    REQUIRE(content.size() == 12);
    CHECK(content[0] == ";$FILEVERSION=2.1\r");
    CHECK(content[1].starts_with(";$STARTTIME="));
}
