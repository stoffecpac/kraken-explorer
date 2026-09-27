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

#include <array>
#include <cstdint>

#include "core/bus_message.h"

// CRC-8H2F lookup table: polynomial 0x2F, non-reflected.
// Used by AUTOSAR E2E Profile 2 (and Profile 4).
namespace detail
{

consteval std::array<uint8_t, 256> make_crc8h2f_table() noexcept
{
    std::array<uint8_t, 256> t{};
    for (int i = 0; i < 256; ++i)
    {
        auto crc = static_cast<uint8_t>(i);
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = (crc & 0x80u) ? static_cast<uint8_t>((crc << 1) ^ 0x2Fu)
                                : static_cast<uint8_t>(crc << 1);
        }
        t[i] = crc;
    }
    return t;
}

inline constexpr auto crc8h2f_table = make_crc8h2f_table();

} // namespace detail

[[nodiscard]] constexpr uint8_t crc8h2f_byte(uint8_t crc, uint8_t byte) noexcept
{
    return detail::crc8h2f_table[crc ^ byte];
}

// Compute AUTOSAR E2E Profile 2 CRC over a BusMessage.
//
// CRC input order (per AUTOSAR_SWS_E2ELibrary):
//   DataID low byte, DataID high byte, data[0]=0x00 (CRC byte zeroed),
//   data[1..len-1]
//
// The message must have len >= 2 (byte 0 = CRC, byte 1 = counter nibble).
// The caller is responsible for writing the returned value into byte 0.
[[nodiscard]] constexpr uint8_t e2e_p2_compute_crc(const BusMessage& msg, uint16_t data_id) noexcept
{
    uint8_t crc = 0xFFu;
    crc = crc8h2f_byte(crc, static_cast<uint8_t>(data_id & 0xFFu));
    crc = crc8h2f_byte(crc, static_cast<uint8_t>(data_id >> 8u));
    crc = crc8h2f_byte(crc, 0x00u); // CRC byte position treated as zero
    for (int i = 1; i < msg.len && i < bus_max_data_bytes; ++i)
    {
        crc = crc8h2f_byte(crc, msg.data[i]);
    }
    return crc ^ 0xFFu;
}
