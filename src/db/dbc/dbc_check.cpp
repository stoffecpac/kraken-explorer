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

#include "dbc_check.h"

#include <algorithm>
#include <bitset>
#include <format>
#include <map>
#include <set>
#include <string>
#include <string_view>

#include "core/bus_message.h"

namespace
{

using PayloadBits = std::bitset<bus_max_data_bytes * 8>;

// The payload bits a signal covers, in the sequential MSB-first numbering that
// CanDbSignal::start_bit uses for Motorola signals. Mirrors extract_raw_signal: a
// Motorola signal runs forward from start_bit; an Intel signal's bits count LSB-first
// within each byte, so bit b of its range is byte b/8, MSB-first position 7 - b%8.
[[nodiscard]] PayloadBits covered_bits(const CanDbSignal& sig)
{
    PayloadBits bits;
    const unsigned end = std::min<unsigned>(sig.start_bit + sig.length, static_cast<unsigned>(bits.size()));
    for (unsigned b = sig.start_bit; b < end; ++b)
    {
        bits.set(sig.big_endian ? b : (b / 8u) * 8u + 7u - (b % 8u));
    }
    return bits;
}

// Two multiplexed signals with different mux values are never on the bus together.
[[nodiscard]] bool may_share_bits(const CanDbSignal& a, const CanDbSignal& b) noexcept
{
    return a.is_muxed && b.is_muxed && a.mux_value != b.mux_value;
}

[[nodiscard]] std::string bit_range(const CanDbSignal& sig)
{
    return std::format("bits {}..{}", sig.start_bit, sig.start_bit + sig.length - 1);
}

void check_message(const CanDbMessage& msg, std::vector<DbcError>& errors)
{
    const auto add = [&errors](std::string message)
    {
        errors.push_back(DbcError{.line = 0, .message = std::move(message)});
    };

    const int payload_bits = msg.dlc * 8;
    const CanDbSignal* muxer = can_db_muxer(msg);
    std::set<std::string_view> names;
    std::vector<PayloadBits> covered;
    covered.reserve(msg.signals.size());
    for (const CanDbSignal& sig : msg.signals)
    {
        covered.push_back(covered_bits(sig));
    }

    for (size_t i = 0; i < msg.signals.size(); ++i)
    {
        const CanDbSignal& sig = msg.signals[i];
        const std::string id = std::format("{}.{}", msg.name, sig.name);

        if (!names.insert(sig.name).second) { add(id + ": duplicate signal name"); }
        if (sig.length == 0)
        {
            add(id + ": length 0");
        }
        else if (sig.start_bit + sig.length > payload_bits)
        {
            add(std::format("{}: {} exceed dlc {} (bits 0..{})", id, bit_range(sig), msg.dlc, payload_bits - 1));
        }
        if (sig.is_muxed && !muxer)
        {
            add(std::format("{}: multiplexed (m{}) but {} has no multiplexer signal", id, sig.mux_value, msg.name));
        }
        if (sig.factor == 0.0) { add(id + ": factor 0"); }
        if (sig.min > sig.max) { add(std::format("{}: min {} > max {}", id, sig.min, sig.max)); }
        if (sig.value_type == SignalValueType::float32 && sig.length != 32)
        {
            add(std::format("{}: float32 needs length 32, has {}", id, sig.length));
        }
        if (sig.value_type == SignalValueType::float64 && sig.length != 64)
        {
            add(std::format("{}: float64 needs length 64, has {}", id, sig.length));
        }

        for (size_t j = i + 1; j < msg.signals.size(); ++j)
        {
            const CanDbSignal& other = msg.signals[j];
            if (may_share_bits(sig, other)) { continue; }
            if ((covered[i] & covered[j]).any())
            {
                add(std::format("{}: {} overlap {} ({})", id, bit_range(sig), other.name, bit_range(other)));
            }
        }
    }
}

} // namespace

std::vector<DbcError> dbc_check(const CanDb& db)
{
    std::vector<DbcError> errors;
    std::map<std::string_view, uint32_t> names;   // message name -> first raw id
    for (const auto& [raw_id, msg] : db.messages)
    {
        if (const auto [it, inserted] = names.try_emplace(msg.name, raw_id); !inserted)
        {
            errors.push_back(DbcError{
                .line = 0,
                .message = std::format("{}: duplicate message name (ids {} and {})", msg.name, it->second, raw_id),
            });
        }
        check_message(msg, errors);
    }
    return errors;
}
