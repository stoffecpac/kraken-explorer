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

// SocketCAN representation of a BusMessage, per <linux/can.h> and
// <linux/can/error.h>: the canid_t with its flag bits and, for error frames,
// the error class bits and 8-byte error payload. Shared by the candump, pcap,
// pcapng and MDF4 writers so every format describes a frame the same way.
//
// Names are lower-case on purpose: <linux/can.h> defines CAN_EFF_FLAG & co as
// macros, which would break these declarations in any file including both.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/bus_message.h"

namespace socket_can
{

// <linux/can.h>
inline constexpr uint32_t eff_flag = 0x80000000U;
inline constexpr uint32_t rtr_flag = 0x40000000U;
inline constexpr uint32_t err_flag = 0x20000000U;
inline constexpr uint32_t eff_mask = 0x1FFFFFFFU;

// <linux/can.h>: sizeof(struct can_frame) / sizeof(struct canfd_frame), canfd_frame::flags
inline constexpr std::size_t can_mtu = 16;
inline constexpr std::size_t canfd_mtu = 72;
inline constexpr uint8_t canfd_brs = 0x01;

// <linux/can/error.h>: error classes in can_id
inline constexpr uint32_t err_tx_timeout = 0x00000001U;
inline constexpr uint32_t err_crtl       = 0x00000004U;
inline constexpr uint32_t err_prot       = 0x00000008U;
inline constexpr uint32_t err_ack        = 0x00000020U;
inline constexpr uint32_t err_busoff     = 0x00000040U;
inline constexpr uint32_t err_buserror   = 0x00000080U;
inline constexpr uint32_t err_restarted  = 0x00000100U;

// <linux/can/error.h>: error payload
inline constexpr int err_dlc = 8;
inline constexpr uint8_t err_crtl_rx_overflow  = 0x01;  // data[1]
inline constexpr uint8_t err_crtl_rx_warning   = 0x04;  // data[1]
inline constexpr uint8_t err_crtl_tx_warning   = 0x08;  // data[1]
inline constexpr uint8_t err_crtl_rx_passive   = 0x10;  // data[1]
inline constexpr uint8_t err_crtl_tx_passive   = 0x20;  // data[1]
inline constexpr uint8_t err_crtl_active       = 0x40;  // data[1]
inline constexpr uint8_t err_prot_bit          = 0x01;  // data[2]
inline constexpr uint8_t err_prot_form         = 0x02;  // data[2]
inline constexpr uint8_t err_prot_stuff        = 0x04;  // data[2]
inline constexpr uint8_t err_prot_loc_crc_seq  = 0x08;  // data[3]

struct ErrorFrame
{
    uint32_t classes = 0;
    std::array<uint8_t, err_dlc> data{};
};

// Error classes and payload for an error frame's bus_error bits.
[[nodiscard]] constexpr ErrorFrame error_frame(const BusMessage& m) noexcept
{
    const uint16_t e = m.errors;
    ErrorFrame error;

    if (e & bus_error::bit)
    {
        error.classes |= err_prot | err_buserror;
        error.data[2] |= err_prot_bit;
    }
    if (e & bus_error::form)
    {
        error.classes |= err_prot | err_buserror;
        error.data[2] |= err_prot_form;
    }
    if (e & bus_error::stuff)
    {
        error.classes |= err_prot | err_buserror;
        error.data[2] |= err_prot_stuff;
    }
    if (e & bus_error::crc)
    {
        error.classes |= err_prot | err_buserror;
        error.data[3] = err_prot_loc_crc_seq;
    }
    if (e & bus_error::ack)
    {
        error.classes |= err_ack | err_buserror;
    }
    if (e & bus_error::bus_off)
    {
        error.classes |= err_busoff;
    }
    if (e & bus_error::overrun)
    {
        error.classes |= err_crtl;
        error.data[1] |= err_crtl_rx_overflow;
    }
    if (e & bus_error::tx_timeout)
    {
        error.classes |= err_tx_timeout;
    }
    if (e & bus_error::restarted)
    {
        error.classes |= err_restarted;
    }
    // bus_error does not record whether TX or RX errors caused the state
    // change, so both directions are flagged.
    if (e & bus_error::error_warning)
    {
        error.classes |= err_crtl;
        error.data[1] |= err_crtl_rx_warning | err_crtl_tx_warning;
    }
    if (e & bus_error::error_passive)
    {
        error.classes |= err_crtl;
        error.data[1] |= err_crtl_rx_passive | err_crtl_tx_passive;
    }
    if (e & bus_error::error_active)
    {
        error.classes |= err_crtl;
        error.data[1] |= err_crtl_active;
    }

    // Generic or otherwise unclassified: an unspecified bus error, which is also
    // how python-can writes error frames. Without any class bit readers such as
    // python-can do not recognise the frame as an error frame at all.
    if (error.classes == 0)
    {
        error.classes = err_buserror;
    }
    return error;
}

// canid_t: identifier plus EFF / RTR flags, or ERR flag plus error classes.
[[nodiscard]] constexpr uint32_t frame_id(const BusMessage& m) noexcept
{
    if (is_error_frame(m))
    {
        return err_flag | error_frame(m).classes;
    }
    uint32_t id = can_id(m);
    if (has_flag(m, bus_flag::extended)) { id |= eff_flag; }
    if (has_flag(m, bus_flag::rtr))      { id |= rtr_flag; }
    return id;
}

// The reverse: a struct can_frame (can_mtu bytes) or canfd_frame (canfd_mtu bytes) in host
// byte order into m: id with the EFF / RTR flags, FD / BRS, length clamped to the frame, data.
// An error frame gets errors = generic and no id. Returns the raw can_id so a caller can
// refine the error classes from it.
inline uint32_t decode_frame(const uint8_t* bytes, std::size_t nbytes, BusMessage& m) noexcept
{
    uint32_t id = 0;
    std::memcpy(&id, bytes, sizeof(id));
    const bool fd = nbytes == canfd_mtu;
    if (id & err_flag)
    {
        m.errors = bus_error::generic;
    }
    else
    {
        m.id = id & eff_mask;
        if (id & eff_flag) { m.flags |= bus_flag::extended; }
        if (id & rtr_flag) { m.flags |= bus_flag::rtr; }
    }
    if (fd)
    {
        m.flags |= bus_flag::fd;
        if (bytes[5] & canfd_brs) { m.flags |= bus_flag::brs; }
    }
    const uint8_t len = std::min<uint8_t>(bytes[4], fd ? 64 : 8);
    set_length(m, len);
    if (!has_flag(m, bus_flag::rtr)) // RTR: len is the requested DLC, the payload bytes are undefined
    {
        std::copy_n(bytes + 8, len, m.data.begin());
    }
    return id;
}

}
