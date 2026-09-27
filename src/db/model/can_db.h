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

// CAN description database (filled by db/dbc/dbc_parser). Plain data plus free functions.
//
// Pointer stability: messages live in a std::map and signals in a std::deque, so a
// CanDbMessage* / CanDbSignal* stays valid across can_db_update_from() (DBC reload)
// for as long as the CanDb itself lives.

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <string_view>

#include "core/bus_message.h"

// DBC SIG_VALTYPE_ 1 / 2, sym float / double: the raw bits are an IEEE-754 value.
enum class SignalValueType : uint8_t
{
    integer,
    float32,    // length 32
    float64,    // length 64
};

struct CanDbSignal
{
    std::string name;
    uint16_t start_bit = 0;     // Motorola: sequential MSB-first index (converted by the parser)
    uint16_t length = 0;
    bool is_unsigned = false;
    bool big_endian = false;
    SignalValueType value_type = SignalValueType::integer;
    double factor = 1.0;
    double offset = 0.0;
    double min = 0.0;
    double max = 0.0;
    std::string unit;
    bool is_muxer = false;
    bool is_muxed = false;
    uint32_t mux_value = 0;
    std::string comment;
    std::map<uint64_t, std::string> value_table;
};

struct CanDbMessage
{
    std::string name;
    uint32_t raw_id = 0;        // as in the DBC: extended ids keep bit 31 set
    uint8_t dlc = 0;
    std::string sender;         // node name
    std::string comment;
    std::deque<CanDbSignal> signals;
    int muxer = -1;             // index into signals, -1 = not multiplexed
};

struct CanDb
{
    std::string path;
    std::map<uint32_t, CanDbMessage> messages;   // keyed by raw_id
};

[[nodiscard]] CanDbMessage* can_db_find_message(CanDb& db, uint32_t raw_id);
[[nodiscard]] inline const CanDbMessage* can_db_find_message(const CanDb& db, uint32_t raw_id)
{
    return can_db_find_message(const_cast<CanDb&>(db), raw_id);
}
[[nodiscard]] CanDbSignal* can_db_find_signal(CanDbMessage& msg, std::string_view name);
[[nodiscard]] inline const CanDbSignal* can_db_find_signal(const CanDbMessage& msg, std::string_view name)
{
    return can_db_find_signal(const_cast<CanDbMessage&>(msg), name);
}
[[nodiscard]] const CanDbSignal* can_db_muxer(const CanDbMessage& msg);

// DBC reload into a database the setup already points at: messages and signals are
// updated in place (matched by raw id / name), the muxer is rebuilt from `other`.
void can_db_update_from(CanDb& db, const CanDb& other);

[[nodiscard]] uint64_t can_signal_extract_raw(const CanDbSignal& sig, const BusMessage& m) noexcept;
[[nodiscard]] double can_signal_raw_to_physical(const CanDbSignal& sig, uint64_t raw) noexcept;
[[nodiscard]] double can_signal_extract_physical(const CanDbSignal& sig, const BusMessage& m) noexcept;
void can_signal_inject_raw(const CanDbSignal& sig, BusMessage& m, uint64_t raw) noexcept;
// Rounds to the nearest raw value; unsigned signals saturate at 0. Float signals store
// (physical - offset) / factor as IEEE bits.
void can_signal_inject_physical(const CanDbSignal& sig, BusMessage& m, double physical) noexcept;
// Printf format for sig's physical values: integer signals with the decimals of factor and offset
// ("%.2f" for 0.01, "%.7f" for 1e-07), so every value is exact and computed ones (B - A, mean)
// carry no float noise; float32 "%.7g", float64 "%.15g".
[[nodiscard]] std::string can_signal_printf(const CanDbSignal& sig);
// A physical value of sig in that format; "-0.00" comes out as "0.00".
[[nodiscard]] std::string can_signal_format(const CanDbSignal& sig, double v);
// Empty when the value has no name.
[[nodiscard]] std::string_view can_signal_value_name(const CanDbSignal& sig, uint64_t value);

// Whether `sig` of `msg` is decodable from frame `m`: id matches, the bits fit in m.len,
// and a muxed signal's mux value equals what the muxer reads.
[[nodiscard]] bool can_signal_present(const CanDbMessage& msg, const CanDbSignal& sig, const BusMessage& m) noexcept;
