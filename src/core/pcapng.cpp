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

#include "pcapng.h"

#include <algorithm>

#include "core/socket_can.h"

namespace
{

constexpr uint32_t bt_shb = 0x0A0D0D0A;
constexpr uint32_t bt_idb = 0x00000001;
constexpr uint32_t bt_epb = 0x00000006;

constexpr uint32_t byte_order_magic = 0x1A2B3C4D;
constexpr uint16_t version_major = 1;
constexpr uint16_t version_minor = 0;

constexpr uint16_t linktype_can_socketcan = 227;
constexpr uint32_t snaplen = 72;

constexpr uint8_t canfd_brs = 0x01;

constexpr uint16_t opt_endofopt = 0;
constexpr uint16_t opt_if_name = 2;
constexpr uint16_t opt_shb_userappl = 4;

constexpr uint32_t can_mtu = 16;
constexpr uint32_t canfd_mtu = 72;

[[nodiscard]] constexpr uint32_t padded4(uint32_t len) noexcept
{
    return (len + 3) & ~uint32_t{3};
}

void pad4(std::vector<uint8_t>& out, uint32_t unpadded)
{
    out.insert(out.end(), padded4(unpadded) - unpadded, 0);
}

// Option: code (2) + length (2) + value + padding.
void append_option(std::vector<uint8_t>& out, uint16_t code, std::string_view value)
{
    append_le(out, code);
    append_le(out, static_cast<uint16_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
    pad4(out, static_cast<uint32_t>(value.size()));
}

void append_end_of_options(std::vector<uint8_t>& out)
{
    append_le(out, opt_endofopt);
    append_le(out, uint16_t{0});
}

}

void append_socketcan_frame(std::vector<uint8_t>& out, const BusMessage& m)
{
    // struct can_frame   { canid_t can_id; __u8 len; __u8 __pad; __u8 __res0; __u8 len8_dlc; __u8 data[8]; };
    // struct canfd_frame { canid_t can_id; __u8 len; __u8 flags; __u8 __res0; __u8 __res1; __u8 data[64]; };
    // can_id is in network byte order for LINKTYPE_CAN_SOCKETCAN. len8_dlc is
    // only meaningful for classic DLC codes above 8, which BusMessage cannot hold.
    const bool error = is_error_frame(m);
    const bool fd = has_flag(m, bus_flag::fd) && !error;
    const size_t begin = out.size();
    out.resize(begin + (fd ? canfd_mtu : can_mtu), 0);
    uint8_t* frame = out.data() + begin;

    const uint32_t id = socket_can::frame_id(m);
    frame[0] = static_cast<uint8_t>(id >> 24);
    frame[1] = static_cast<uint8_t>(id >> 16);
    frame[2] = static_cast<uint8_t>(id >> 8);
    frame[3] = static_cast<uint8_t>(id);

    if (error)
    {
        // Error frames are classic frames carrying the 8-byte error payload.
        const socket_can::ErrorFrame e = socket_can::error_frame(m);
        frame[4] = static_cast<uint8_t>(socket_can::err_dlc);
        std::copy(e.data.begin(), e.data.end(), frame + 8);
        return;
    }

    const uint8_t len = std::min<uint8_t>(m.len, fd ? 64 : 8);
    frame[4] = len;
    if (fd && has_flag(m, bus_flag::brs))
    {
        frame[5] = canfd_brs;
    }
    std::copy_n(m.data.begin(), len, frame + 8);
}

void pcapng_append_section_header(std::vector<uint8_t>& out)
{
    constexpr std::string_view app_name = "CANgaroo";
    constexpr uint32_t opt_len = 4 + padded4(app_name.size()) + 4;  // userappl + endofopt
    // type(4) + total_length(4) + bom(4) + major(2) + minor(2) + section_length(8) + options + total_length(4)
    constexpr uint32_t total_len = 4 + 4 + 4 + 2 + 2 + 8 + opt_len + 4;

    append_le(out, bt_shb);
    append_le(out, total_len);
    append_le(out, byte_order_magic);
    append_le(out, version_major);
    append_le(out, version_minor);
    append_le(out, UINT64_MAX);  // section length: unspecified
    append_option(out, opt_shb_userappl, app_name);
    append_end_of_options(out);
    append_le(out, total_len);
}

void pcapng_append_interface(std::vector<uint8_t>& out, std::string_view name)
{
    const uint32_t opt_len = 4 + padded4(static_cast<uint32_t>(name.size())) + 4;  // if_name + endofopt
    // type(4) + total_length(4) + linktype(2) + reserved(2) + snaplen(4) + options + total_length(4)
    const uint32_t total_len = 4 + 4 + 2 + 2 + 4 + opt_len + 4;

    append_le(out, bt_idb);
    append_le(out, total_len);
    append_le(out, linktype_can_socketcan);
    append_le(out, uint16_t{0});  // reserved
    append_le(out, snaplen);
    append_option(out, opt_if_name, name);
    append_end_of_options(out);
    append_le(out, total_len);
}

void pcapng_append_packet(std::vector<uint8_t>& out, const BusMessage& m, uint32_t iface_index)
{
    // Timestamp in the interface's ts_resol, default microseconds.
    const auto ts_us = static_cast<uint64_t>(m.ts_ns / 1000);
    const uint32_t captured_len = (has_flag(m, bus_flag::fd) && !is_error_frame(m)) ? canfd_mtu : can_mtu;
    // type(4) + total_length(4) + interface_id(4) + ts_high(4) + ts_low(4)
    // + captured_len(4) + original_len(4) + packet data (padded) + total_length(4)
    const uint32_t total_len = 4 + 4 + 4 + 4 + 4 + 4 + 4 + padded4(captured_len) + 4;

    append_le(out, bt_epb);
    append_le(out, total_len);
    append_le(out, iface_index);
    append_le(out, static_cast<uint32_t>(ts_us >> 32));
    append_le(out, static_cast<uint32_t>(ts_us));
    append_le(out, captured_len);
    append_le(out, captured_len);  // original length
    append_socketcan_frame(out, m);
    pad4(out, captured_len);
    append_le(out, total_len);
}
