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

#include "lin_db.h"

#include <algorithm>
#include <ranges>
#include <type_traits>
#include <unordered_map>

#include "db/ldf/ldf_parser.h"

namespace
{

// Fills the resolved fields of a schedule entry; an unknown frame keeps the defaults.
void resolve_entry(LinScheduleEntry& entry, const ldf::LdfFile& ldf)
{
    if (const auto f = std::ranges::find(ldf.frames, entry.frame_name, &ldf::Frame::name); f != ldf.frames.end())
    {
        entry.frame_id = f->id;
        entry.dlc = f->length;
        entry.publisher_name = f->publisher;
        entry.is_master_publisher = (f->publisher == ldf.nodes.master);
    }
}

[[nodiscard]] LinScheduleTable convert_schedule(const ldf::ScheduleTable& tbl, const ldf::LdfFile& ldf)
{
    std::unordered_map<std::string, const ldf::SporadicFrame*> sporadic_by_name;
    for (const auto& sf : ldf.sporadic_frames)
    {
        sporadic_by_name.emplace(sf.name, &sf);
    }

    LinScheduleTable out{.name = tbl.name};
    for (const auto& e : tbl.entries)
    {
        if (e.frame_name.empty())
        {
            continue; // MasterReq / SlaveResp / configuration command
        }
        const auto delay_ms = static_cast<uint8_t>(std::clamp(e.delay_s * 1000.0, 0.0, 255.0));

        // A sporadic group expands to one entry per underlying unconditional frame.
        if (const auto it = sporadic_by_name.find(e.frame_name); it != sporadic_by_name.end())
        {
            for (const auto& name : it->second->frames)
            {
                LinScheduleEntry entry{.frame_name = name, .delay_ms = delay_ms, .is_sporadic = true};
                resolve_entry(entry, ldf);
                out.entries.push_back(std::move(entry));
            }
            continue;
        }

        LinScheduleEntry entry{.frame_name = e.frame_name, .delay_ms = delay_ms};
        resolve_entry(entry, ldf);
        out.entries.push_back(std::move(entry));
    }
    return out;
}

} // namespace

bool lin_db_load(LinDb& db, const std::string& path)
{
    auto result = ldf::parse_file(path);
    if (!result)
    {
        db.last_error = result.error();
        return false;
    }
    const ldf::LdfFile& ldf = *result;

    LinDb out{
        .path = path,
        .protocol_version = ldf.lin_protocol_version,
        .speed_bps = ldf.lin_speed_bps,
        .master_node = ldf.nodes.master,
        .slave_nodes = ldf.nodes.slaves,
        .master_timebase_ms = ldf.nodes.master_time_base_s * 1000.0,
        .master_jitter_ms = ldf.nodes.master_jitter_s * 1000.0,
    };

    for (const auto& attr : ldf.node_attributes)
    {
        out.diag_timings[attr.name] = LinDiagTiming{
            .p2_min_ms = static_cast<uint16_t>(attr.p2_min_s * 1000.0),
            .st_min_ms = static_cast<uint16_t>(attr.st_min_s * 1000.0),
            .n_as_ms = static_cast<uint16_t>(attr.n_as_timeout_s * 1000.0),
            .n_cr_ms = static_cast<uint16_t>(attr.n_cr_timeout_s * 1000.0),
        };
        // Prefer configured_NAD, then initial_NAD, then NAD.
        out.node_nads[attr.name] = attr.configured_nad ? *attr.configured_nad
                                 : attr.initial_nad    ? *attr.initial_nad
                                                       : attr.nad;
    }

    for (const auto& tbl : ldf.schedule_tables)
    {
        out.schedule_tables.push_back(convert_schedule(tbl, ldf));
    }

    std::unordered_map<std::string, const ldf::SignalEncodingType*> sig_encoding;
    for (const auto& [enc_name, sig_names] : ldf.signal_representation)
    {
        const auto enc = std::ranges::find(ldf.signal_encoding_types, enc_name, &ldf::SignalEncodingType::name);
        if (enc == ldf.signal_encoding_types.end())
        {
            continue;
        }
        for (const auto& sig_name : sig_names)
        {
            sig_encoding.emplace(sig_name, &*enc);
        }
    }

    std::unordered_map<std::string, const ldf::Signal*> sig_defs;
    for (const auto& sig : ldf.signals)
    {
        sig_defs.emplace(sig.name, &sig);
    }

    for (const auto& ldf_frame : ldf.frames)
    {
        LinFrame frame{
            .id = ldf_frame.id,
            .name = ldf_frame.name,
            .publisher = ldf_frame.publisher,
            .length = ldf_frame.length,
        };

        for (const auto& ref : ldf_frame.signals)
        {
            LinSignal sig{.name = ref.signal_name, .bit_offset = static_cast<uint8_t>(ref.bit_offset)};

            if (const auto it = sig_defs.find(ref.signal_name); it != sig_defs.end())
            {
                sig.bit_length = static_cast<uint8_t>(it->second->bit_length);
                sig.publisher = it->second->publisher;
                sig.init_value = it->second->init_value;
            }

            if (const auto it = sig_encoding.find(ref.signal_name); it != sig_encoding.end())
            {
                for (const auto& enc_val : it->second->values)
                {
                    std::visit(
                        [&](const auto& v)
                        {
                            using T = std::decay_t<decltype(v)>;
                            if constexpr (std::is_same_v<T, ldf::PhysicalRange>)
                            {
                                sig.factor = v.scale;
                                sig.offset = v.offset;
                                sig.min = v.min_value;
                                sig.max = v.max_value;
                                sig.unit = v.unit;
                            }
                            else if constexpr (std::is_same_v<T, ldf::LogicalValue>)
                            {
                                sig.value_table[v.signal_value] = v.text;
                            }
                        },
                        enc_val);
                }
            }
            frame.signals.push_back(std::move(sig));
        }
        out.frames[ldf_frame.id] = std::move(frame);
    }

    db = std::move(out);
    return true;
}

const LinFrame* lin_db_find_frame(const LinDb& db, std::string_view name)
{
    const auto frames = db.frames | std::views::values;
    const auto it = std::ranges::find(frames, name, &LinFrame::name);
    return it != frames.end() ? &*it : nullptr;
}

std::string lin_db_diag_node(const LinDb& db, bool master, std::string_view slave)
{
    return master ? (db.slave_nodes.empty() ? std::string{} : db.slave_nodes.front()) : std::string(slave);
}

LinDiagTiming lin_db_diag_timing(const LinDb& db, std::string_view node)
{
    const auto it = db.diag_timings.find(node);
    return (it != db.diag_timings.end()) ? it->second : LinDiagTiming{};
}

uint8_t lin_db_node_nad(const LinDb& db, std::string_view node)
{
    const auto it = db.node_nads.find(node);
    return (it != db.node_nads.end()) ? it->second : 0;
}

const LinSignal* lin_frame_find_signal(const LinFrame& frame, std::string_view name)
{
    const auto it = std::ranges::find(frame.signals, name, &LinSignal::name);
    return it != frame.signals.end() ? &*it : nullptr;
}

uint64_t lin_signal_extract_raw(const LinSignal& sig, std::span<const uint8_t> data) noexcept
{
    uint64_t raw = 0;
    for (int i = 0; i < sig.bit_length && i < 64; ++i)
    {
        const uint32_t bit_pos = sig.bit_offset + static_cast<uint32_t>(i);
        const uint32_t byte_idx = bit_pos / 8;
        if (byte_idx < data.size() && ((data[byte_idx] >> (bit_pos % 8)) & 1u))
        {
            raw |= uint64_t{1} << i;
        }
    }
    return raw;
}

double lin_signal_raw_to_physical(const LinSignal& sig, uint64_t raw) noexcept
{
    return static_cast<double>(raw) * sig.factor + sig.offset;
}

double lin_signal_extract_physical(const LinSignal& sig, std::span<const uint8_t> data) noexcept
{
    return lin_signal_raw_to_physical(sig, lin_signal_extract_raw(sig, data));
}

std::string_view lin_signal_value_name(const LinSignal& sig, uint64_t value)
{
    const auto it = sig.value_table.find(value);
    return (it != sig.value_table.end()) ? std::string_view(it->second) : std::string_view();
}
