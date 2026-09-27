/*
  Copyright (c) 2015, 2016 Hubert Denkmair <hubert@denkmair.de>
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

#include "trace_file_writer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <ostream>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/pcapng.h"
#include "core/socket_can.h"
#include "core/trace_line_format.h"

// VERSION_STRING is a global compile definition for the application; keep a fallback.
#ifndef VERSION_STRING
#define VERSION_STRING "dev"
#endif

namespace
{

void write_bytes(std::ostream& out, const std::vector<uint8_t>& bytes)
{
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void append_raw(std::vector<uint8_t>& out, std::string_view s)
{
    out.insert(out.end(), s.begin(), s.end());
}

// TXBLOCK / MDBLOCK size: 24-byte header + text + NUL, padded to 8 bytes.
[[nodiscard]] constexpr uint64_t mdf_text_block_size(std::string_view text) noexcept
{
    return (24 + static_cast<uint64_t>(text.size()) + 1 + 7) & ~uint64_t{7};
}

struct MdfChannel
{
    std::string_view name;
    std::string_view unit;
    uint8_t type;        // cn_type: 0 = fixed length, 2 = master
    uint8_t sync_type;   // cn_sync_type: 0 = none, 1 = time
    uint8_t data_type;   // cn_data_type: 0 = UINT LE, 4 = REAL LE, 10 = byte array
    uint32_t byte_offset;
    uint32_t bit_count;
    uint64_t offset = 0;
    uint64_t name_offset = 0;
    uint64_t unit_offset = 0;
};

constexpr uint32_t mdf_record_size = 8 + 4 + 1 + 1 + 64;  // t, CAN_ID, DLC, Dir, DataBytes

[[nodiscard]] int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// Linux candump -L lines.
void write_candump(std::ostream& out, std::span<const BusMessage> messages, const IfaceNameFn& iface_name)
{
    std::string line;
    for (const BusMessage& m : messages)
    {
        line.clear();
        append_candump_line(line, m, iface_name(m.iface));
        line += '\n';
        out << line;
    }
}

// Vector ASC; times relative to the earliest message (the trace may be unsorted), channels numbered 1.. in order
// of first appearance. An empty trace still gets header and footer.
void write_vector_asc(std::ostream& out, std::span<const BusMessage> messages)
{
    using namespace std::chrono;
    const int64_t start_ns = messages.empty() ? now_ns() : std::ranges::min(messages, {}, &BusMessage::ts_ns).ts_ns;

    std::string line;
    append_asc_header(line, system_clock::time_point(duration_cast<system_clock::duration>(nanoseconds(start_ns))));
    out << line;

    std::unordered_map<uint16_t, int> channels;
    for (const BusMessage& m : messages)
    {
        const auto [it, inserted] = channels.try_emplace(m.iface, static_cast<int>(channels.size()) + 1);
        line.clear();
        append_asc_line(line, m, start_ns, it->second);
        line += '\n';
        out << line;
    }

    out << asc_footer << '\n';
}

// PEAK PCAN trace 2.1; like ASC, times relative to the earliest message and buses numbered 1.. in order of first
// appearance. LIN frames have no TRC representation and are left out.
void write_trc(std::ostream& out, std::span<const BusMessage> messages)
{
    const int64_t start_ns = messages.empty() ? now_ns() : std::ranges::min(messages, {}, &BusMessage::ts_ns).ts_ns;

    std::string line;
    append_trc_header(line, start_ns);
    out << line;

    std::unordered_map<uint16_t, int> buses;
    uint64_t number = 0;
    for (const BusMessage& m : messages)
    {
        if (m.type != BusType::CAN)
        {
            continue;
        }
        const auto [it, inserted] = buses.try_emplace(m.iface, static_cast<int>(buses.size()) + 1);
        line.clear();
        append_trc_line(line, m, ++number, start_ns, it->second);
        line += "\r\n";
        out << line;
    }
}

// ASAM MDF 4.10 with one channel group of fixed-size records:
//   t (REAL, seconds since the first message, master)
//   | CAN_ID (canid_t, see core/socket_can.h: flags, error classes for error frames)
//   | DLC (payload length) | Dir (0 = Rx, 1 = Tx) | DataBytes (64-byte array)
void write_vector_mdf(std::ostream& out, std::span<const BusMessage> messages)
{
    // The master channel t must be non-decreasing; frames of several interfaces may not be.
    std::vector<BusMessage> sorted;
    if (!std::ranges::is_sorted(messages, {}, &BusMessage::ts_ns))
    {
        sorted.assign(messages.begin(), messages.end());
        std::ranges::stable_sort(sorted, {}, &BusMessage::ts_ns);
        messages = sorted;
    }

    // Block sizes follow the ASAM MDF 4.1 block definitions: 24-byte common
    // header, 8 bytes per link, then the block's data section.
    constexpr uint64_t sz_id = 64;
    constexpr uint64_t sz_hd = 104;  // 6 links + 32 data
    constexpr uint64_t sz_fh = 56;   // 2 links + 16 data
    constexpr uint64_t sz_dg = 64;   // 4 links + 8 data
    constexpr uint64_t sz_cg = 104;  // 6 links + 32 data
    constexpr uint64_t sz_cn = 160;  // 8 links + 72 data

    const auto record_count = static_cast<uint64_t>(messages.size());
    // Microsecond resolution, as the trace timestamps always had.
    const uint64_t start_time_ns = messages.empty()
        ? static_cast<uint64_t>(now_ns() / 1000000) * 1000000
        : static_cast<uint64_t>(messages.front().ts_ns / 1000) * 1000;
    const int64_t t_start_ns = messages.empty() ? 0 : messages.front().ts_ns;

    // The file history comment must be an MDBLOCK with <FHcomment>.
    const std::string fh_comment =
        std::string("<FHcomment xmlns=\"http://www.asam.net/mdf/v4\"><TX>Kraken Explorer trace export</TX>"
                    "<tool_id>Kraken Explorer</tool_id><tool_vendor>Kraken Explorer</tool_vendor><tool_version>")
        + VERSION_STRING + "</tool_version></FHcomment>";
    constexpr std::string_view acquisition_name = "CAN";

    std::array<MdfChannel, 5> channels = {{
        { "t",         "s", 2, 1, 4,  0,  64  },
        { "CAN_ID",    "",  0, 0, 0,  8,  32  },
        { "DLC",       "",  0, 0, 0,  12, 8   },
        { "Dir",       "",  0, 0, 0,  13, 8   },
        { "DataBytes", "",  0, 0, 10, 14, 512 },
    }};

    // --- Block offsets ---
    uint64_t off = sz_id + sz_hd;
    const uint64_t o_fh = off;      off += sz_fh;
    const uint64_t o_md_fh = off;   off += mdf_text_block_size(fh_comment);
    const uint64_t o_dg = off;      off += sz_dg;
    const uint64_t o_cg = off;      off += sz_cg;
    const uint64_t o_tx_cg = off;   off += mdf_text_block_size(acquisition_name);
    for (MdfChannel& ch : channels)
    {
        ch.offset = off;            off += sz_cn;
        ch.name_offset = off;       off += mdf_text_block_size(ch.name);
        if (!ch.unit.empty())
        {
            ch.unit_offset = off;   off += mdf_text_block_size(ch.unit);
        }
    }
    const uint64_t o_dt = off;

    std::vector<uint8_t> b;
    b.reserve(static_cast<size_t>(o_dt + 24 + record_count * mdf_record_size));

    auto zeros = [&b](size_t count) { b.insert(b.end(), count, 0); };
    auto block_header = [&b](std::string_view id, uint64_t length, uint64_t link_count)
    {
        append_raw(b, id);
        append_le(b, uint32_t{0});  // reserved
        append_le(b, length);
        append_le(b, link_count);
    };
    auto text_block = [&](std::string_view id, std::string_view text)
    {
        const uint64_t size = mdf_text_block_size(text);
        block_header(id, size, 0);
        append_raw(b, text);
        zeros(static_cast<size_t>(size - 24 - text.size()));  // NUL terminator + padding
    };

    // ===== IDBLOCK (no common header) =====
    append_raw(b, "MDF     ");
    append_raw(b, "4.10    ");
    append_raw(b, "Kraken  ");  // id_prog: 8 chars, space padded
    zeros(4);                        // id_reserved1
    append_le(b, uint16_t{410});     // id_ver
    zeros(30);                       // id_reserved2
    append_le(b, uint16_t{0});       // id_unfin_flags: finalized
    append_le(b, uint16_t{0});       // id_custom_unfin_flags

    // ===== HDBLOCK =====
    block_header("##HD", sz_hd, 6);
    append_le(b, o_dg);              // hd_dg_first
    append_le(b, o_fh);              // hd_fh_first
    append_le(b, uint64_t{0});       // hd_ch_first
    append_le(b, uint64_t{0});       // hd_at_first
    append_le(b, uint64_t{0});       // hd_ev_first
    append_le(b, uint64_t{0});       // hd_md_comment
    append_le(b, start_time_ns);     // hd_start_time_ns (UTC)
    append_le(b, int16_t{0});        // hd_tz_offset_min
    append_le(b, int16_t{0});        // hd_dst_offset_min
    append_le(b, uint8_t{0});        // hd_time_flags: UTC, offsets not valid
    append_le(b, uint8_t{0});        // hd_time_class: local PC reference time
    append_le(b, uint8_t{0});        // hd_flags: start angle / distance not valid
    append_le(b, uint8_t{0});        // hd_reserved
    append_le(b, 0.0);               // hd_start_angle_rad
    append_le(b, 0.0);               // hd_start_distance_m

    // ===== FHBLOCK =====
    block_header("##FH", sz_fh, 2);
    append_le(b, uint64_t{0});       // fh_fh_next
    append_le(b, o_md_fh);           // fh_md_comment
    append_le(b, start_time_ns);     // fh_time_ns
    append_le(b, int16_t{0});        // fh_tz_offset_min
    append_le(b, int16_t{0});        // fh_dst_offset_min
    append_le(b, uint8_t{0});        // fh_time_flags
    zeros(3);                        // fh_reserved
    text_block("##MD", fh_comment);

    // ===== DGBLOCK =====
    block_header("##DG", sz_dg, 4);
    append_le(b, uint64_t{0});       // dg_dg_next
    append_le(b, o_cg);              // dg_cg_first
    append_le(b, o_dt);              // dg_data
    append_le(b, uint64_t{0});       // dg_md_comment
    append_le(b, uint8_t{0});        // dg_rec_id_size: single channel group
    zeros(7);                        // dg_reserved

    // ===== CGBLOCK =====
    block_header("##CG", sz_cg, 6);
    append_le(b, uint64_t{0});       // cg_cg_next
    append_le(b, channels[0].offset);// cg_cn_first
    append_le(b, o_tx_cg);           // cg_tx_acq_name
    append_le(b, uint64_t{0});       // cg_si_acq_source
    append_le(b, uint64_t{0});       // cg_sr_first
    append_le(b, uint64_t{0});       // cg_md_comment
    append_le(b, uint64_t{0});       // cg_record_id
    append_le(b, record_count);      // cg_cycle_count
    append_le(b, uint16_t{0});       // cg_flags
    append_le(b, uint16_t{0});       // cg_path_separator
    zeros(4);                        // cg_reserved
    append_le(b, mdf_record_size);   // cg_data_bytes
    append_le(b, uint32_t{0});       // cg_inval_bytes
    text_block("##TX", acquisition_name);

    // ===== CNBLOCKs =====
    for (size_t i = 0; i < channels.size(); ++i)
    {
        const MdfChannel& ch = channels[i];
        block_header("##CN", sz_cn, 8);
        append_le(b, i + 1 < channels.size() ? channels[i + 1].offset : uint64_t{0});  // cn_cn_next
        append_le(b, uint64_t{0});   // cn_composition
        append_le(b, ch.name_offset);// cn_tx_name
        append_le(b, uint64_t{0});   // cn_si_source
        append_le(b, uint64_t{0});   // cn_cc_conversion
        append_le(b, uint64_t{0});   // cn_data
        append_le(b, ch.unit_offset);// cn_md_unit
        append_le(b, uint64_t{0});   // cn_md_comment
        append_le(b, ch.type);
        append_le(b, ch.sync_type);
        append_le(b, ch.data_type);
        append_le(b, uint8_t{0});    // cn_bit_offset
        append_le(b, ch.byte_offset);
        append_le(b, ch.bit_count);
        append_le(b, uint32_t{0});   // cn_flags: ranges and limits not valid
        append_le(b, uint32_t{0});   // cn_inval_bit_pos
        append_le(b, uint8_t{0});    // cn_precision
        append_le(b, uint8_t{0});    // cn_reserved
        append_le(b, uint16_t{0});   // cn_attachment_count
        for (int k = 0; k < 6; ++k)
        {
            append_le(b, 0.0);       // value range, limit, extended limit
        }
        text_block("##TX", ch.name);
        if (!ch.unit.empty())
        {
            text_block("##TX", ch.unit);
        }
    }

    // ===== DTBLOCK =====
    block_header("##DT", 24 + record_count * mdf_record_size, 0);
    for (const BusMessage& m : messages)
    {
        const uint8_t len = std::min<uint8_t>(m.len, 64);
        // canid_t as in <linux/can.h>: standard and extended frames with the same
        // number stay distinct, error frames keep their error class.
        // Microsecond resolution, as the trace timestamps always had.
        append_le(b, static_cast<double>(m.ts_ns / 1000 - t_start_ns / 1000) / 1e6);
        append_le(b, socket_can::frame_id(m));
        append_le(b, len);
        append_le(b, uint8_t{has_flag(m, bus_flag::tx) ? uint8_t{1} : uint8_t{0}});
        b.insert(b.end(), m.data.begin(), m.data.begin() + len);
        zeros(64 - len);
    }
    write_bytes(out, b);
}

// libpcap, microsecond timestamps, LINKTYPE_CAN_SOCKETCAN.
void write_pcap(std::ostream& out, std::span<const BusMessage> messages)
{
    std::vector<uint8_t> b;
    append_le(b, uint32_t{0xA1B2C3D4});  // magic: microsecond timestamps
    append_le(b, uint16_t{2});
    append_le(b, uint16_t{4});
    append_le(b, int32_t{0});            // thiszone: UTC
    append_le(b, uint32_t{0});           // sigfigs
    append_le(b, uint32_t{72});          // snaplen: CANFD_MTU
    append_le(b, uint32_t{227});         // LINKTYPE_CAN_SOCKETCAN

    std::vector<uint8_t> frame;
    for (const BusMessage& m : messages)
    {
        frame.clear();
        append_socketcan_frame(frame, m);
        const int64_t ts_us = m.ts_ns / 1000;
        append_le(b, static_cast<uint32_t>(ts_us / 1000000));
        append_le(b, static_cast<uint32_t>(ts_us % 1000000));
        append_le(b, static_cast<uint32_t>(frame.size()));  // incl_len
        append_le(b, static_cast<uint32_t>(frame.size()));  // orig_len
        b.insert(b.end(), frame.begin(), frame.end());
    }
    write_bytes(out, b);
}

// pcapng: one section, one interface description per interface in order of
// first appearance, then one enhanced packet per message.
void write_pcapng(std::ostream& out, std::span<const BusMessage> messages, const IfaceNameFn& iface_name)
{
    std::unordered_map<uint16_t, uint32_t> index;
    std::vector<uint16_t> order;
    for (const BusMessage& m : messages)
    {
        if (index.try_emplace(m.iface, static_cast<uint32_t>(order.size())).second)
        {
            order.push_back(m.iface);
        }
    }

    std::vector<uint8_t> b;
    pcapng_append_section_header(b);
    for (const uint16_t iface : order)
    {
        pcapng_append_interface(b, iface_name(iface));
    }
    for (const BusMessage& m : messages)
    {
        pcapng_append_packet(b, m, index[m.iface]);
    }
    write_bytes(out, b);
}

}

void write_trace_file(std::ostream& out, TraceFileFormat format, std::span<const BusMessage> messages,
                      const IfaceNameFn& iface_name)
{
    switch (format)
    {
        case TraceFileFormat::CanDump:   write_candump(out, messages, iface_name); return;
        case TraceFileFormat::VectorAsc: write_vector_asc(out, messages); return;
        case TraceFileFormat::VectorMdf: write_vector_mdf(out, messages); return;
        case TraceFileFormat::Pcap:      write_pcap(out, messages); return;
        case TraceFileFormat::PcapNg:    write_pcapng(out, messages, iface_name); return;
        case TraceFileFormat::Trc:       write_trc(out, messages); return;
    }
}
