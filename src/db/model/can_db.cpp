/*

  Copyright (c) 2015, 2016 Hubert Denkmair <hubert@denkmair.de>
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

#include "can_db.h"

#include <algorithm>
#include <bit>
#include <cmath>

CanDbMessage* can_db_find_message(CanDb& db, uint32_t raw_id)
{
    const auto it = db.messages.find(raw_id);
    return (it != db.messages.end()) ? &it->second : nullptr;
}

CanDbSignal* can_db_find_signal(CanDbMessage& msg, std::string_view name)
{
    const auto it = std::ranges::find(msg.signals, name, &CanDbSignal::name);
    return it != msg.signals.end() ? &*it : nullptr;
}

const CanDbSignal* can_db_muxer(const CanDbMessage& msg)
{
    if (msg.muxer < 0 || static_cast<size_t>(msg.muxer) >= msg.signals.size())
    {
        return nullptr;
    }
    return &msg.signals[static_cast<size_t>(msg.muxer)];
}

void can_db_update_from(CanDb& db, const CanDb& other)
{
    for (const auto& [raw_id, other_msg] : other.messages)
    {
        CanDbMessage& my_msg = db.messages[raw_id];
        my_msg.raw_id = raw_id;
        my_msg.name = other_msg.name;
        my_msg.dlc = other_msg.dlc;
        my_msg.comment = other_msg.comment;

        // The muxer is what can_signal_present() consults to decide whether a muxed
        // signal is decoded at all; copying the per-signal is_muxer flag does not
        // establish it. Rebuilt from `other`, cleared first so a message that lost its
        // multiplexer does not keep pointing at a stale signal.
        my_msg.muxer = -1;

        for (const auto& other_sig : other_msg.signals)
        {
            const auto idx = static_cast<size_t>(
                std::ranges::find(my_msg.signals, other_sig.name, &CanDbSignal::name) - my_msg.signals.begin());
            if (idx == my_msg.signals.size())
            {
                my_msg.signals.push_back(CanDbSignal{.name = other_sig.name});
            }
            CanDbSignal& my_sig = my_msg.signals[idx];
            // Everything but the value table, as before the port.
            auto value_table = std::move(my_sig.value_table);
            my_sig = other_sig;
            my_sig.value_table = std::move(value_table);

            if (other_sig.is_muxer)
            {
                my_msg.muxer = static_cast<int>(idx);
            }
        }
    }
}

uint64_t can_signal_extract_raw(const CanDbSignal& sig, const BusMessage& m) noexcept
{
    return extract_raw_signal(m, sig.start_bit, sig.length, sig.big_endian);
}

double can_signal_raw_to_physical(const CanDbSignal& sig, uint64_t raw) noexcept
{
    if (sig.value_type == SignalValueType::float32 && sig.length == 32)
    {
        return static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(raw))) * sig.factor + sig.offset;
    }
    if (sig.value_type == SignalValueType::float64 && sig.length == 64)
    {
        return std::bit_cast<double>(raw) * sig.factor + sig.offset;
    }
    // A length outside 1..64 bits cannot be sign-extended: the shift width would be
    // 64 (or negative), which is undefined behaviour. DBC files are not validated on
    // that field, so fall back to the unsigned reading.
    if (sig.is_unsigned || sig.length == 0 || sig.length > 64)
    {
        return static_cast<double>(raw) * sig.factor + sig.offset;
    }
    int64_t v = static_cast<int64_t>(raw << (64 - sig.length));
    v >>= (64 - sig.length);
    return static_cast<double>(v) * sig.factor + sig.offset;
}

double can_signal_extract_physical(const CanDbSignal& sig, const BusMessage& m) noexcept
{
    return can_signal_raw_to_physical(sig, can_signal_extract_raw(sig, m));
}

void can_signal_inject_raw(const CanDbSignal& sig, BusMessage& m, uint64_t raw) noexcept
{
    inject_raw_signal(m, sig.start_bit, sig.length, sig.big_endian, raw);
}

void can_signal_inject_physical(const CanDbSignal& sig, BusMessage& m, double physical) noexcept
{
    const double raw_double = (physical - sig.offset) / sig.factor;
    uint64_t raw;
    if (sig.value_type == SignalValueType::float32 && sig.length == 32)
    {
        raw = std::bit_cast<uint32_t>(static_cast<float>(raw_double));
    }
    else if (sig.value_type == SignalValueType::float64 && sig.length == 64)
    {
        raw = std::bit_cast<uint64_t>(raw_double);
    }
    else if (sig.is_unsigned)
    {
        raw = static_cast<uint64_t>(std::round(raw_double < 0.0 ? 0.0 : raw_double));
    }
    else
    {
        raw = static_cast<uint64_t>(static_cast<int64_t>(std::round(raw_double)));
    }
    can_signal_inject_raw(sig, m, raw);
}

std::string_view can_signal_value_name(const CanDbSignal& sig, uint64_t value)
{
    const auto it = sig.value_table.find(value);
    return (it != sig.value_table.end()) ? std::string_view(it->second) : std::string_view();
}

bool can_signal_present(const CanDbMessage& msg, const CanDbSignal& sig, const BusMessage& m) noexcept
{
    if (m.id != (msg.raw_id & can_id_mask_extended))
    {
        return false;
    }
    if (sig.start_bit + sig.length > 8 * m.len)
    {
        return false;
    }
    if (!sig.is_muxed)
    {
        return true;
    }
    const CanDbSignal* muxer = can_db_muxer(msg);
    return muxer && sig.mux_value == can_signal_extract_raw(*muxer, m);
}
