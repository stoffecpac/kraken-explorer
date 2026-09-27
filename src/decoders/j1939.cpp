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

#include <algorithm>
#include <format>

#include "decoders/decoders.h"

namespace
{

// RX and TX traffic get independent session slots (see session_key() in
// uds.cpp for why), and the interface/channel is folded in too, since SA/DA
// pairs can collide across separate buses. DA is part of the key so concurrent
// BAM and RTS sessions from the same SA stay apart.
[[nodiscard]] uint64_t tp_session_key(const BusMessage& frame) noexcept
{
    const uint8_t sa = frame.id & 0xFF;
    const uint8_t da = (frame.id >> 8) & 0xFF;
    uint32_t key32 = (static_cast<uint32_t>(sa) << 8) | da;
    if (has_flag(frame, bus_flag::tx))
    {
        key32 |= 0x10000u;
    }
    return (static_cast<uint64_t>(frame.iface) << 32) | key32;
}

void fill_header_metadata(ProtocolMessage& out, uint32_t id)
{
    out.metadata.clear();
    out.metadata["Priority"] = (id >> 26) & 0x7;
    out.metadata["Reserved"] = (id >> 25) & 1;
    out.metadata["Data Page"] = (id >> 24) & 1;
    out.metadata["PDU Format"] = (id >> 16) & 0xFF;
    out.metadata["PDU Specific"] = (id >> 8) & 0xFF;
    out.metadata["Source Address"] = id & 0xFF;
}

} // namespace

DecodeStatus j1939_decode(J1939Decoder& d, const BusMessage& frame, ProtocolMessage& out)
{
    if (is_error_frame(frame) || has_flag(frame, bus_flag::rtr) || !has_flag(frame, bus_flag::extended))
    {
        return DecodeStatus::Ignored;
    }

    const int len = std::min<int>(frame.len, bus_max_data_bytes);
    const auto& b = frame.data;
    const uint32_t id = frame.id;
    const uint32_t pgn = j1939_pgn(id);
    const uint64_t key = tp_session_key(frame);

    if (pgn == 0x00EC00) // TP.CM
    {
        if (len < 8)
        {
            return DecodeStatus::Ignored;
        }
        const uint8_t control = b[0];

        if (control == 32 || control == 16) // BAM (32) or RTS (16)
        {
            auto& s = d.sessions[key];
            s.pgn = b[5] | (static_cast<uint32_t>(b[6]) << 8) | (static_cast<uint32_t>(b[7]) << 16);
            s.expected_size = b[1] | (b[2] << 8);
            s.expected_packets = b[3];
            s.received_packets = 0;
            s.data.clear();
            s.frames.assign(1, frame);
            return DecodeStatus::Consumed;
        }

        // CTS (17), End of Message Ack (19), Connection Abort (255)
        if (control == 255)
        {
            d.sessions.erase(key);
        }
        return d.sessions.contains(key) ? DecodeStatus::Consumed : DecodeStatus::Ignored;
    }

    if (pgn == 0x00EB00) // TP.DT
    {
        const auto it = d.sessions.find(key);
        if (it == d.sessions.end())
        {
            return DecodeStatus::Ignored;
        }
        auto& s = it->second;

        // J1939-21: DT byte 0 = packet number, 1-based
        if (len < 1 || b[0] != static_cast<uint8_t>(s.received_packets + 1))
        {
            d.sessions.erase(it);
            return DecodeStatus::Ignored;
        }

        s.received_packets++;
        s.frames.push_back(frame);

        // Guard against malformed DT frames with DLC < 8
        for (int i = 1; i < std::min(8, len) && static_cast<int>(s.data.size()) < s.expected_size; ++i)
        {
            s.data.push_back(b[i]);
        }

        if (s.received_packets < s.expected_packets && static_cast<int>(s.data.size()) < s.expected_size)
        {
            return DecodeStatus::Consumed;
        }

        out.ts_ns = s.frames.front().ts_ns;
        out.protocol = "J1939";
        out.id = s.pgn;
        out.type = MessageType::Request;
        out.name = j1939_pgn_name(s.pgn);
        out.description.clear();
        fill_header_metadata(out, s.frames.front().id); // from the TP.CM header frame
        out.payload = std::move(s.data);
        out.raw_frames = std::move(s.frames);
        d.sessions.erase(it);
        return DecodeStatus::Completed;
    }

    // Single-packet PGN
    const uint8_t sa = id & 0xFF;
    const uint8_t pf = (id >> 16) & 0xFF;
    const uint8_t ps = (id >> 8) & 0xFF;

    // PGNs 0xDA00/0xDB00 (PDU1) are reserved for peer-to-peer diagnostic
    // communication. ISO 15765-4/UDS-on-CAN extended addressing reuses
    // this exact 29-bit ID scheme (e.g. 0x18DAxxyy), so single frames
    // here are UDS traffic, not generic J1939 data - let the UDS decoder
    // have a chance at them instead of swallowing them as J1939.
    if (pf == 0xDA || pf == 0xDB)
    {
        return DecodeStatus::Ignored;
    }

    out.payload.assign(b.begin(), b.begin() + len);
    out.raw_frames.assign(1, frame);
    out.protocol = "J1939";
    out.ts_ns = frame.ts_ns;
    out.id = pgn;
    out.type = MessageType::Request;
    out.name = j1939_pgn_name(pgn);
    out.description = (pf < 240)
        ? std::format("PDU1 (Peer-to-Peer) from SA [0x{:02x}] to DA [0x{:02x}]", sa, ps)
        : std::format("PDU2 (Broadcast) from SA [0x{:02x}]", sa);
    fill_header_metadata(out, id);
    return DecodeStatus::Completed;
}

uint32_t j1939_pgn(uint32_t id) noexcept
{
    const uint32_t pf = (id >> 16) & 0xFF;
    uint32_t pgn = (id >> 8) & 0x3FF00; // EDP, DP and PF
    if (pf >= 240)
    {
        pgn |= (id >> 8) & 0xFF; // PDU2: PS is a group extension
    }
    return pgn;
}

std::string j1939_pgn_name(uint32_t pgn)
{
    switch (pgn)
    {
        case 0xFEEC: return "Vehicle Identification (VIN)";
        case 0xF004: return "Electronic Engine Controller 1 (EEC1)";
        case 0xFEEF: return "Engine Fluid Level/Pressure";
        case 0xFEE5: return "Engine Hours/Revolutions";
        case 0xFEF1: return "Cruise Control/Vehicle Speed";
        case 0xF000: return "Torque/Speed Control";
        case 0xFECA: return "Active Diagnostic Trouble Codes";
        case 0xFECB: return "Previously Active DTCs";
        case 0xFEDA: return "Software Identification";
        case 0xFEDB: return "ECU Identification Information";
        default:     return std::format("PGN: 0x{:04x}", pgn);
    }
}
