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

// One CAN / CAN FD / LIN frame as a trivially copyable POD. No heap, no strings:
// the interface is an index that is resolved to a name only when displayed.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

enum class BusType : uint8_t
{
    CAN = 0,
    LIN = 1,
};

// BusMessage::flags bits
namespace bus_flag
{
inline constexpr uint16_t extended    = 0x0001;
inline constexpr uint16_t rtr         = 0x0002;
inline constexpr uint16_t fd          = 0x0004;
inline constexpr uint16_t brs         = 0x0008;
inline constexpr uint16_t tx          = 0x0010;   // zero-initialised frame is RX
inline constexpr uint16_t lin_sleep   = 0x0020;
inline constexpr uint16_t lin_wakeup  = 0x0040;
}

// BusMessage::errors bits; a frame with any bit set is an error frame
namespace bus_error
{
inline constexpr uint16_t ack                = 0x0001;
inline constexpr uint16_t bit                = 0x0002;
inline constexpr uint16_t stuff              = 0x0004;
inline constexpr uint16_t crc                = 0x0008;
inline constexpr uint16_t form               = 0x0010;
inline constexpr uint16_t bus_off            = 0x0020;
inline constexpr uint16_t overrun            = 0x0040;
inline constexpr uint16_t tx_timeout         = 0x0080;
inline constexpr uint16_t lin_not_responded  = 0x0100;
inline constexpr uint16_t lin_checksum_error = 0x0200;
inline constexpr uint16_t restarted          = 0x0400;   // controller recovered from bus-off
inline constexpr uint16_t error_warning      = 0x0800;   // controller reached the error-warning level
inline constexpr uint16_t error_passive      = 0x1000;   // controller reached the error-passive level
inline constexpr uint16_t error_active       = 0x2000;   // controller back to error-active
inline constexpr uint16_t generic            = 0x8000;
}

inline constexpr int bus_max_data_bytes = 64;   // CAN FD maximum payload
inline constexpr uint32_t can_id_mask_standard = 0x7FF;
inline constexpr uint32_t can_id_mask_extended = 0x1FFFFFFF;

struct BusMessage
{
    uint32_t id = 0;        // identifier only, no flag bits (LIN: frame id 0..63)
    uint16_t flags = 0;     // bus_flag::*
    uint16_t errors = 0;    // bus_error::*
    uint16_t iface = 0;     // interface index, name looked up on display
    uint8_t dlc = 0;        // DLC code 0..15 (CAN FD table), kept in sync by set_length()
    uint8_t len = 0;        // payload bytes 0..64
    BusType type = BusType::CAN;
    int64_t ts_ns = 0;      // timestamp, ns since the Unix epoch
    std::array<uint8_t, bus_max_data_bytes> data{};
};
static_assert(std::is_trivially_copyable_v<BusMessage>);

// CAN FD DLC code (0x0-0xF) -> payload length in bytes, ISO 11898-1:2015 Table 5.
inline constexpr std::array<uint8_t, 16> bus_dlc_lengths = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };

// Smallest DLC code whose length holds `length` bytes (15 for anything above 48).
[[nodiscard]] constexpr uint8_t bus_length_to_dlc(int length) noexcept
{
    uint8_t dlc = 0;
    while (dlc < 15 && bus_dlc_lengths[dlc] < length)
    {
        ++dlc;
    }
    return dlc;
}

[[nodiscard]] constexpr bool has_flag(const BusMessage& m, uint16_t flag) noexcept { return (m.flags & flag) != 0; }
[[nodiscard]] constexpr bool is_error_frame(const BusMessage& m) noexcept { return m.errors != 0; }

// Sets len and dlc together. Lengths above 64 fall back to 8, as before the port.
constexpr void set_length(BusMessage& m, int length) noexcept
{
    m.len = static_cast<uint8_t>((length >= 0 && length <= bus_max_data_bytes) ? length : 8);
    m.dlc = bus_length_to_dlc(m.len);
}

// Standard ids are masked to 11 bits, extended to 29.
[[nodiscard]] constexpr uint32_t can_id(const BusMessage& m) noexcept
{
    return m.id & (has_flag(m, bus_flag::extended) ? can_id_mask_extended : can_id_mask_standard);
}

// Raw signal access. Big-endian (Motorola) start bits are the sequential MSB-first
// bit index DbcParser produces; little-endian ones the usual Intel LSB offset.
[[nodiscard]] uint64_t extract_raw_signal(const BusMessage& m, uint16_t start_bit, uint16_t length, bool big_endian) noexcept;
// Bits beyond m.len are dropped rather than written.
void inject_raw_signal(BusMessage& m, uint16_t start_bit, uint16_t length, bool big_endian, uint64_t value) noexcept;

// Formatters append to a caller-owned buffer, so a reused string does not allocate per row.
// "0x123", "0x1ABCDEF0" (extended) or "0x3C" (LIN), upper-case hex.
void append_id(std::string& out, const BusMessage& m);
// Upper-case hex with `sep` between bytes: "0142FF", "01 42 FF".
void append_hex_bytes(std::string& out, std::span<const uint8_t> bytes, std::string_view sep = {});
// "01 42 FF" (hex) or "A . ! . " (ascii: '.' for non-printables, one space after each byte).
void append_bytes(std::string& out, std::span<const uint8_t> bytes, bool ascii);
// "ERROR: ACK+CRC", or "ERROR" when no known bit is set.
void append_error_flags(std::string& out, uint16_t errors);
// Payload column: nothing for an empty frame, the error flags for an error frame, else append_bytes().
void append_data(std::string& out, const BusMessage& m, bool ascii);
