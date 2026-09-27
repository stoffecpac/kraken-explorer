#include "ui/replay.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>

#include <imgui.h>

#include "app.h"
#include "core/text.h"
#include "core/trace_line_format.h"
#include "ui/depth_gauge.h"
#include "ui/icons.h"

namespace
{

// ---------------------------------------------------------------- parsing helpers

// "12.345678" -> ns, exact for up to 9 fractional digits.
bool parse_seconds_ns(std::string_view s, int64_t& ns)
{
    const auto dot = s.find('.');
    int64_t sec = 0;
    if (!parse_number(s.substr(0, dot), sec))
    {
        return false;
    }
    int64_t frac = 0;
    if (dot != std::string_view::npos)
    {
        std::string_view f = s.substr(dot + 1, 9);
        if (!f.empty() && !parse_number(f, frac))
        {
            return false;
        }
        for (std::size_t i = f.size(); i < 9; ++i)
        {
            frac *= 10;
        }
    }
    ns = sec * 1000000000 + frac;
    return true;
}

std::vector<std::string_view> split_ws(std::string_view line)
{
    std::vector<std::string_view> parts;
    std::size_t i = 0;
    while (i < line.size())
    {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
        {
            ++i;
        }
        const std::size_t b = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
        {
            ++i;
        }
        if (i > b)
        {
            parts.push_back(line.substr(b, i - b));
        }
    }
    return parts;
}

uint16_t channel_index(ReplayFile& f, std::string name)
{
    const auto it = std::ranges::find(f.channels, name);
    if (it != f.channels.end())
    {
        return static_cast<uint16_t>(it - f.channels.begin());
    }
    f.channels.push_back(std::move(name));
    return static_cast<uint16_t>(f.channels.size() - 1);
}

bool parse_hex_bytes(std::string_view hex, BusMessage& m)
{
    if (hex.size() % 2 != 0 || hex.size() / 2 > bus_max_data_bytes)
    {
        return false;
    }
    for (std::size_t i = 0; i < hex.size() / 2; ++i)
    {
        if (!parse_number(hex.substr(i * 2, 2), m.data[i], 16))
        {
            return false;
        }
    }
    set_length(m, static_cast<int>(hex.size() / 2));
    return true;
}

// Every 4096 calls: publishes done / total and returns false on a stop request.
bool parse_tick(const ReplayParseProgress& p, std::size_t& n, std::size_t done, std::size_t total)
{
    if (++n % 4096 != 0)
    {
        return true;
    }
    if (p.fraction != nullptr && total > 0)
    {
        p.fraction->store(static_cast<float>(static_cast<double>(done) / static_cast<double>(total)), std::memory_order_relaxed);
    }
    return !p.stop.stop_requested();
}

template <class F>
void for_each_line(std::string_view data, const ReplayParseProgress& p, F&& fn)
{
    const std::size_t total = data.size();
    std::size_t n = 0;
    while (!data.empty() && parse_tick(p, n, total - data.size(), total))
    {
        const auto nl = data.find('\n');
        fn(data.substr(0, nl));
        data = nl == std::string_view::npos ? std::string_view{} : data.substr(nl + 1);
    }
}

// "(ts) iface ID#DATA", "ID##<flags>DATA" (FD), "ID#R[len]" (RTR); error flag in the id.
void parse_candump(std::string_view data, ReplayFile& f, const ReplayParseProgress& p)
{
    for_each_line(data, p, [&](std::string_view line)
    {
        const auto parts = split_ws(line);
        if (parts.size() < 3 || parts[0].size() < 3 || parts[0].front() != '(' || parts[0].back() != ')')
        {
            return;
        }
        BusMessage m;
        if (!parse_seconds_ns(parts[0].substr(1, parts[0].size() - 2), m.ts_ns))
        {
            return;
        }
        const std::string_view frame = parts[2];
        const auto hash = frame.find('#');
        uint32_t raw = 0;
        if (hash == std::string_view::npos || !parse_number(frame.substr(0, hash), raw, 16))
        {
            return;
        }
        std::string_view payload = frame.substr(hash + 1);
        if ((raw & 0x20000000u) != 0) // CAN_ERR_FLAG
        {
            m.errors = bus_error::generic;
            m.id = raw & can_id_mask_extended;
        }
        else
        {
            m.id = raw & can_id_mask_extended;
            if (hash == 8 || m.id > can_id_mask_standard)
            {
                m.flags |= bus_flag::extended;
            }
            if (!payload.empty() && payload.front() == '#')
            {
                uint8_t fd_flags = 0;
                if (payload.size() < 2 || !parse_number(payload.substr(1, 1), fd_flags, 16))
                {
                    return;
                }
                m.flags |= bus_flag::fd | ((fd_flags & 1) != 0 ? bus_flag::brs : 0);
                if (!parse_hex_bytes(payload.substr(2), m))
                {
                    return;
                }
            }
            else if (!payload.empty() && (payload.front() == 'R' || payload.front() == 'r'))
            {
                int len = 0;
                if (payload.size() > 1 && !parse_number(payload.substr(1), len))
                {
                    return;
                }
                m.flags |= bus_flag::rtr;
                set_length(m, std::min(len, 8));
            }
            else if (!parse_hex_bytes(payload, m) || m.len > 8)
            {
                return;
            }
        }
        m.iface = channel_index(f, std::string(parts[1]));
        f.frames.push_back(m);
    });
}

// Vector ASC events: classic, CANFD, LIN and ErrorFrame lines; everything else is skipped.
void parse_asc(std::string_view data, ReplayFile& f, const ReplayParseProgress& p)
{
    for_each_line(data, p, [&](std::string_view line)
    {
        const auto parts = split_ws(line);
        BusMessage m;
        if (parts.size() < 3 || !parse_seconds_ns(parts[0], m.ts_ns))
        {
            return;
        }
        std::string channel(parts[1]);
        if (iequals(parts[2], "ErrorFrame"))
        {
            m.errors = bus_error::generic;
        }
        else if (iequals(parts[1], "CANFD"))
        {
            const int64_t ts = m.ts_ns;
            if (!parse_asc_canfd_line(parts, m))
            {
                return;
            }
            m.ts_ns = ts;
            channel = std::string(parts[2]);
        }
        else if (iequals(parts[2], "LIN") && parts.size() >= 6)
        {
            // <t> <ch> LIN <id> <Rx|Tx> d <len> <bytes...> checksum = ..., or LIN_<error>
            m.type = BusType::LIN;
            if (!parse_number(parts[3], m.id, 16))
            {
                return;
            }
            m.id &= 0x3F;
            m.flags |= iequals(parts[4], "Tx") ? bus_flag::tx : 0;
            if (parts[5].starts_with("LIN_"))
            {
                m.errors = bus_error::lin_checksum_error;
            }
            else
            {
                int len = 0;
                if (!iequals(parts[5], "d") || parts.size() < 7 || !parse_number(parts[6], len) || len > 8)
                {
                    return;
                }
                set_length(m, len);
                for (int i = 0; i < len && 7 + i < static_cast<int>(parts.size()); ++i)
                {
                    if (!parse_number(parts[7 + i], m.data[i], 16))
                    {
                        break; // "checksum"
                    }
                }
            }
        }
        else
        {
            // <t> <ch> <id>[x] <Rx|Tx> <d|r> <dlc> <bytes...>
            if (parts.size() < 6)
            {
                return;
            }
            std::string_view id = parts[2];
            if (id.ends_with('x') || id.ends_with('X'))
            {
                id.remove_suffix(1);
                m.flags |= bus_flag::extended;
            }
            int len = 0;
            if (!parse_number(id, m.id, 16) || !parse_number(parts[5], len) || len > 8)
            {
                return;
            }
            m.flags |= iequals(parts[3], "Tx") ? bus_flag::tx : 0;
            m.flags |= iequals(parts[4], "r") ? bus_flag::rtr : 0;
            set_length(m, len);
            if (!has_flag(m, bus_flag::rtr))
            {
                for (int i = 0; i < len; ++i)
                {
                    if (6 + i >= static_cast<int>(parts.size()) || !parse_number(parts[6 + i], m.data[i], 16))
                    {
                        return;
                    }
                }
            }
        }
        m.iface = channel_index(f, "CH " + channel);
        f.frames.push_back(m);
    });
}

// PEAK PCAN trace: ";" header / comment lines, then one frame per line (see parse_trc_line).
void parse_trc(std::string_view data, ReplayFile& f, const ReplayParseProgress& p)
{
    TrcLayout layout;
    for_each_line(data, p, [&](std::string_view line)
    {
        if (line.starts_with(';'))
        {
            parse_trc_header_line(line, layout);
            return;
        }
        BusMessage m;
        if (parse_trc_line(split_ws(line), layout, m))
        {
            m.iface = channel_index(f, "CH " + std::to_string(m.iface));
            f.frames.push_back(m);
        }
    });
}

// Bounds-checked little/big-endian reads from the file bytes.
struct Bytes
{
    std::string_view d;
    bool swap = false;

    [[nodiscard]] bool has(std::size_t off, std::size_t n) const { return off <= d.size() && n <= d.size() - off; }
    [[nodiscard]] uint32_t u32(std::size_t off) const
    {
        const auto* p = reinterpret_cast<const uint8_t*>(d.data() + off);
        return swap ? (uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3])
                    : (uint32_t{p[3]} << 24 | uint32_t{p[2]} << 16 | uint32_t{p[1]} << 8 | p[0]);
    }
    [[nodiscard]] uint16_t u16(std::size_t off) const
    {
        const auto* p = reinterpret_cast<const uint8_t*>(d.data() + off);
        return static_cast<uint16_t>(swap ? (p[0] << 8 | p[1]) : (p[1] << 8 | p[0]));
    }
};

constexpr uint32_t linktype_socketcan = 227;

// struct can_frame / canfd_frame; can_id in network byte order (LINKTYPE_CAN_SOCKETCAN).
bool parse_socketcan(std::string_view pkt, BusMessage& m)
{
    if (pkt.size() < 8)
    {
        return false;
    }
    const auto* p = reinterpret_cast<const uint8_t*>(pkt.data());
    const uint32_t can_id = uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3];
    const bool fd = pkt.size() == 72;
    m.id = can_id & can_id_mask_extended;
    m.flags |= (can_id & 0x80000000u) != 0 ? bus_flag::extended : 0;
    if ((can_id & 0x20000000u) != 0)
    {
        m.errors = bus_error::generic;
        return true;
    }
    m.flags |= (can_id & 0x40000000u) != 0 ? bus_flag::rtr : 0;
    if (fd)
    {
        m.flags |= bus_flag::fd | ((p[5] & 1) != 0 ? bus_flag::brs : 0);
    }
    const int len = std::min<int>(p[4], fd ? 64 : 8);
    if (len > static_cast<int>(pkt.size()) - 8)
    {
        return false;
    }
    set_length(m, len);
    if (!has_flag(m, bus_flag::rtr))
    {
        std::memcpy(m.data.data(), p + 8, static_cast<std::size_t>(len));
    }
    return true;
}

void parse_pcap(std::string_view data, ReplayFile& f, const ReplayParseProgress& p)
{
    Bytes b{data};
    if (!b.has(0, 24))
    {
        return;
    }
    const uint32_t magic = b.u32(0);
    bool nano = false;
    if (magic == 0xA1B2C3D4 || magic == 0xA1B23C4D)
    {
        nano = magic == 0xA1B23C4D;
    }
    else if (magic == 0xD4C3B2A1 || magic == 0x4D3CB2A1)
    {
        b.swap = true;
        nano = magic == 0x4D3CB2A1;
    }
    else
    {
        return;
    }
    if (b.u32(20) != linktype_socketcan)
    {
        return;
    }
    const uint16_t channel = channel_index(f, "pcap0");
    std::size_t n = 0;
    for (std::size_t off = 24; b.has(off, 16) && parse_tick(p, n, off, data.size());)
    {
        const uint32_t incl = b.u32(off + 8);
        if (!b.has(off + 16, incl))
        {
            break;
        }
        BusMessage m;
        m.ts_ns = int64_t{b.u32(off)} * 1000000000 + int64_t{b.u32(off + 4)} * (nano ? 1 : 1000);
        if (parse_socketcan(data.substr(off + 16, incl), m))
        {
            m.iface = channel;
            f.frames.push_back(m);
        }
        off += 16 + incl;
    }
}

void parse_pcapng(std::string_view data, ReplayFile& f, const ReplayParseProgress& p)
{
    struct Idb
    {
        uint16_t channel = 0;
        uint16_t link = 0;
        uint8_t tsresol = 6;
    };
    std::vector<Idb> idbs;
    Bytes b{data};
    std::size_t n = 0;
    for (std::size_t off = 0; b.has(off, 12) && parse_tick(p, n, off, data.size());)
    {
        if (b.u32(off) == 0x0A0D0D0A) // SHB: its byte-order magic decides the endianness
        {
            b.swap = false;
            if (b.u32(off + 8) != 0x1A2B3C4D)
            {
                b.swap = true;
                if (b.u32(off + 8) != 0x1A2B3C4D)
                {
                    return;
                }
            }
            idbs.clear();
        }
        const uint32_t type = b.u32(off);
        const uint32_t total = b.u32(off + 4);
        if (total < 12 || total % 4 != 0 || !b.has(off, total))
        {
            return;
        }
        if (type == 1 && total >= 20) // IDB: link type, options if_name (2), if_tsresol (9)
        {
            Idb idb{.link = b.u16(off + 8)};
            std::string name = std::format("if{}", idbs.size());
            for (std::size_t o = off + 16; o + 4 <= off + total - 4;)
            {
                const uint16_t code = b.u16(o);
                const uint16_t len = b.u16(o + 2);
                if (code == 0 || o + 4 + len > off + total - 4)
                {
                    break;
                }
                if (code == 2 && len > 0)
                {
                    name = std::string(data.substr(o + 4, len));
                    name.erase(std::ranges::find(name, '\0'), name.end());
                }
                else if (code == 9 && len == 1)
                {
                    idb.tsresol = static_cast<uint8_t>(data[o + 4]);
                }
                o += 4 + ((len + 3u) & ~3u);
            }
            idb.channel = channel_index(f, std::move(name));
            idbs.push_back(idb);
        }
        else if (type == 6 && total >= 32) // EPB
        {
            const uint32_t if_id = b.u32(off + 8);
            const uint32_t incl = b.u32(off + 20);
            if (if_id < idbs.size() && idbs[if_id].link == linktype_socketcan && 28 + uint64_t{incl} <= total)
            {
                const Idb& idb = idbs[if_id];
                const uint64_t raw = uint64_t{b.u32(off + 12)} << 32 | b.u32(off + 16);
                BusMessage m;
                if ((idb.tsresol & 0x80) == 0 && idb.tsresol <= 9)
                {
                    uint64_t scale = 1;
                    for (int i = idb.tsresol; i < 9; ++i)
                    {
                        scale *= 10;
                    }
                    m.ts_ns = static_cast<int64_t>(raw * scale);
                }
                else // ponytail: power-of-2 or sub-ns resolutions, via double; exact integer math if a capture uses them
                {
                    const double unit = (idb.tsresol & 0x80) != 0 ? 1.0 / static_cast<double>(1ull << (idb.tsresol & 0x7F))
                                                                   : 1.0 / std::pow(10.0, idb.tsresol);
                    m.ts_ns = static_cast<int64_t>(static_cast<double>(raw) * unit * 1e9);
                }
                if (parse_socketcan(data.substr(off + 28, incl), m))
                {
                    m.iface = idb.channel;
                    f.frames.push_back(m);
                }
            }
        }
        off += total;
    }
}

// ---------------------------------------------------------------- UI helpers

bool row_enabled(const std::vector<ReplayIdRow>& rows, const BusMessage& m)
{
    const uint32_t id = is_error_frame(m) ? replay_error_id : m.id;
    const auto it = std::ranges::lower_bound(rows, std::pair{m.iface, id}, {},
                                             [](const ReplayIdRow& r) { return std::pair{r.channel, r.id}; });
    if (it == rows.end() || it->channel != m.iface || it->id != id)
    {
        return false;
    }
    return has_flag(m, bus_flag::tx) ? it->tx_on : it->rx_on;
}

bool channel_is_lin(const ReplayFile& f, uint16_t channel)
{
    return std::ranges::any_of(f.frames, [&](const BusMessage& m) { return m.iface == channel && m.type == BusType::LIN; });
}

void draw_filter_table(App& app, Replay& r)
{
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                      | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("##filter", 6, flags))
    {
        return;
    }
    const float px = ImGui::GetFontSize() / 15.0f;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Interface / CAN ID", ImGuiTableColumnFlags_WidthFixed, 150.0f * px);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("RX", ImGuiTableColumnFlags_WidthFixed, 30.0f * px);
    ImGui::TableSetupColumn("TX", ImGuiTableColumnFlags_WidthFixed, 30.0f * px);
    ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 60.0f * px);
    ImGui::TableSetupColumn("Output", ImGuiTableColumnFlags_WidthFixed, 150.0f * px);
    ImGui::TableHeadersRow();

    for (uint16_t ch = 0; ch < r.data.file.channels.size(); ++ch)
    {
        ImGui::PushID(ch);
        const bool lin = r.data.channel_lin[ch] != 0;
        const auto first = std::ranges::find_if(r.data.rows, [&](const ReplayIdRow& row) { return row.channel == ch; });
        const auto last = std::find_if(first, r.data.rows.end(), [&](const ReplayIdRow& row) { return row.channel != ch; });
        int total = 0;
        bool all_on = true;
        for (auto it = first; it != last; ++it)
        {
            total += it->count;
            all_on = all_on && (it->rx_on || !it->has_rx) && (it->tx_on || !it->has_tx);
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const bool expanded = ImGui::TreeNodeEx("##ch", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAllColumns);
        ImGui::SameLine();
        bool on = all_on;
        if (ImGui::Checkbox(std::format("{} ({})", r.data.file.channels[ch], lin ? "LIN" : "CAN").c_str(), &on))
        {
            for (auto it = first; it != last; ++it)
            {
                it->rx_on = on && it->has_rx;
                it->tx_on = on && it->has_tx;
            }
        }
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%d", total);
        ImGui::TableSetColumnIndex(5);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const int target = r.mapping[ch];
        const std::string preview = target >= 0 && static_cast<std::size_t>(target) < app.ifaces.size()
            ? app.ifaces[static_cast<std::size_t>(target)].info.name : "Trace only";
        ImGui::BeginDisabled(lin);
        if (ImGui::BeginCombo("##out", preview.c_str()))
        {
            if (ImGui::Selectable("Trace only", target < 0))
            {
                r.mapping[ch] = replay_trace_only;
            }
            for (const Iface& iface : app.ifaces)
            {
                if (iface.info.bus_type != BusType::LIN && ImGui::Selectable(iface.info.name.c_str(), target == iface.index))
                {
                    r.mapping[ch] = iface.index;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (lin)
        {
            ImGui::SetItemTooltip("LIN frames can only be replayed to the trace");
        }

        if (expanded)
        {
            for (auto it = first; it != last; ++it)
            {
                ReplayIdRow& row = *it;
                ImGui::PushID(static_cast<int>(row.id));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Indent();
                if (row.id == replay_error_id)
                {
                    ImGui::TextUnformatted("ERROR");
                }
                else
                {
                    ImGui::Text("0x%X", row.id);
                }
                ImGui::Unindent();
                ImGui::TableNextColumn();
                if (row.id != replay_error_id && !lin)
                {
                    // Looked up in the DBC of the mapped interface's network (trace only: any network).
                    const BusMessage probe{.id = row.id,
                                           .flags = row.extended ? bus_flag::extended : uint16_t{0},
                                           .iface = static_cast<uint16_t>(target >= 0 ? target : UINT16_MAX)};
                    if (const CanDbMessage* db = setup_find_can_message(app.setup, probe))
                    {
                        ImGui::TextUnformatted(db->name.c_str());
                    }
                }
                ImGui::TableNextColumn();
                if (row.has_rx)
                {
                    ImGui::Checkbox("##rx", &row.rx_on);
                }
                ImGui::TableNextColumn();
                if (row.has_tx)
                {
                    ImGui::Checkbox("##tx", &row.tx_on);
                }
                ImGui::TableNextColumn();
                ImGui::Text("%d", row.count);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

} // namespace

ReplayFile replay_parse(std::string_view data, TraceFileFormat format, const ReplayParseProgress& progress)
{
    ReplayFile f;
    switch (format)
    {
        case TraceFileFormat::CanDump:   parse_candump(data, f, progress); break;
        case TraceFileFormat::VectorAsc: parse_asc(data, f, progress); break;
        case TraceFileFormat::Pcap:      parse_pcap(data, f, progress); break;
        case TraceFileFormat::PcapNg:    parse_pcapng(data, f, progress); break;
        case TraceFileFormat::Trc:       parse_trc(data, f, progress); break;
        case TraceFileFormat::VectorMdf: break;
    }
    return f;
}

std::vector<ReplayIdRow> replay_id_rows(const ReplayFile& file)
{
    std::vector<ReplayIdRow> rows;
    for (const BusMessage& m : file.frames)
    {
        const uint32_t id = is_error_frame(m) ? replay_error_id : m.id;
        auto it = std::ranges::find_if(rows, [&](const ReplayIdRow& r) { return r.channel == m.iface && r.id == id; });
        if (it == rows.end())
        {
            it = rows.insert(rows.end(), {.channel = m.iface, .id = id, .extended = has_flag(m, bus_flag::extended)});
        }
        ++it->count;
        (has_flag(m, bus_flag::tx) ? it->has_tx : it->has_rx) = true;
    }
    for (ReplayIdRow& r : rows)
    {
        r.rx_on = r.has_rx;
        r.tx_on = r.has_tx;
    }
    std::ranges::sort(rows, {}, [](const ReplayIdRow& r) { return std::pair{r.channel, r.id}; });
    return rows;
}

std::vector<ReplayStep> replay_plan(const ReplayFile& file, const std::vector<ReplayIdRow>& rows,
                                    const std::vector<int>& mapping)
{
    std::vector<ReplayStep> plan;
    if (file.frames.empty())
    {
        return plan;
    }
    const int64_t t0 = file.frames.front().ts_ns;
    for (const BusMessage& m : file.frames)
    {
        if (!row_enabled(rows, m))
        {
            continue;
        }
        const int mapped = m.iface < mapping.size() ? mapping[m.iface] : replay_trace_only;
        const bool sendable = m.type == BusType::CAN && !is_error_frame(m);
        plan.push_back({.msg = m, .at_ns = std::max<int64_t>(0, m.ts_ns - t0), .target = sendable ? mapped : replay_trace_only});
    }
    return plan;
}

void replay_run(std::stop_token stop, Replay& r, std::deque<Iface>& ifaces, Tasks& tasks, double speed, bool loop)
{
    using namespace std::chrono;
    std::mutex mutex;
    std::condition_variable_any cv; // only to sleep until the deadline or the stop request
    do
    {
        const auto start = steady_clock::now();
        for (std::size_t i = 0; i < r.plan.size(); ++i)
        {
            const ReplayStep& step = r.plan[i];
            const auto due = start + duration_cast<steady_clock::duration>(duration<double, std::nano>(static_cast<double>(step.at_ns) / speed));
            {
                std::unique_lock lock(mutex);
                cv.wait_until(lock, stop, due, [] { return false; });
            }
            if (stop.stop_requested())
            {
                r.running = false;
                return;
            }
            BusMessage msg = step.msg;
            msg.flags &= static_cast<uint16_t>(~bus_flag::tx);
            if (step.target >= 0 && static_cast<std::size_t>(step.target) < ifaces.size())
            {
                msg.iface = static_cast<uint16_t>(step.target);
                if (iface_send(ifaces[static_cast<std::size_t>(step.target)], msg))
                {
                    r.position = i + 1; // the TX echo puts it in the trace
                    continue;
                }
            }
            // Trace only, or the interface is closed: show it in the trace as the old window did.
            msg.ts_ns = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
            tasks_post(tasks, [msg](App& app) { trace_append(app.trace, {&msg, 1}); });
            r.position = i + 1;
        }
    } while (loop && !r.plan.empty() && r.plan.back().at_ns > 0 && !stop.stop_requested());
    r.running = false;
}

namespace
{

// Loader thread body: reads the file in chunks, parses it and hands the result over through
// done. A stop request ends it without a result (only after replay_load_cancel dropped the future).
void replay_load_run(std::stop_token stop, std::promise<ReplayLoaded> done, Replay& r, std::string path, void (*wake)())
{
    ReplayLoaded out;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        out.info = "Error: Cannot open file.";
    }
    else
    {
        std::string data;
        std::error_code ec;
        data.reserve(static_cast<std::size_t>(std::filesystem::file_size(path, ec)));
        std::vector<char> chunk(std::size_t{1} << 22);
        while (in.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) || in.gcount() > 0)
        {
            data.append(chunk.data(), static_cast<std::size_t>(in.gcount()));
            if (stop.stop_requested())
            {
                return;
            }
        }
        out.file = replay_parse(data, trace_format_from_path(path).value_or(TraceFileFormat::VectorAsc),
                                {.stop = stop, .fraction = &r.load_fraction});
        if (stop.stop_requested())
        {
            return;
        }
        if (out.file.frames.empty())
        {
            out.info = "Error: Failed to parse trace file or file is empty.";
        }
        else
        {
            out.rows = replay_id_rows(out.file);
            for (uint16_t ch = 0; ch < out.file.channels.size(); ++ch)
            {
                out.channel_lin.push_back(channel_is_lin(out.file, ch));
            }
            std::vector<double> gaps(out.file.frames.size() - 1);
            for (std::size_t i = 0; i < gaps.size(); ++i)
            {
                gaps[i] = static_cast<double>(out.file.frames[i + 1].ts_ns - out.file.frames[i].ts_ns) / 1e9;
            }
            out.gaps = stats_of(gaps);
            const double duration = static_cast<double>(out.file.frames.back().ts_ns - out.file.frames.front().ts_ns) / 1e9;
            out.info = std::format("File: {}\nMessages: {}\nDuration: {:.3f} s", std::filesystem::path(path).filename().string(),
                                   out.file.frames.size(), duration);
            out.path = std::move(path);
        }
    }
    r.load_fraction = 1.0f;
    done.set_value(std::move(out));
    if (wake != nullptr)
    {
        wake();
    }
}

} // namespace

void replay_load_cancel(Replay& r)
{
    r.loader = {}; // request_stop + join
    r.loading = {};
}

void replay_load(App& app, Replay& r, const std::string& path)
{
    replay_stop(r);
    replay_load_cancel(r);
    r.data = {};
    r.mapping.clear();
    r.plan.clear();
    r.position = 0;
    r.load_fraction = 0.0f;
    r.data.info = std::format("Diving into {}...", std::filesystem::path(path).filename().string());
    std::promise<ReplayLoaded> done;
    r.loading = done.get_future();
    r.loader = std::jthread(replay_load_run, std::move(done), std::ref(r), path, app.tasks.wake);
}

bool replay_load_poll(App& app, Replay& r)
{
    if (!r.loading.valid() || r.loading.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        return false;
    }
    r.data = r.loading.get();
    r.loader.join();
    // Default output: the interface with the channel's name (e.g. candump "vcan0"), else trace only.
    for (uint16_t ch = 0; ch < r.data.file.channels.size(); ++ch)
    {
        int target = replay_trace_only;
        for (const Iface& iface : app.ifaces)
        {
            if (iface.info.name == r.data.file.channels[ch] && iface.info.bus_type == BusType::CAN && r.data.channel_lin[ch] == 0)
            {
                target = iface.index;
            }
        }
        r.mapping.push_back(target);
    }
    if (r.autoplay && app.measuring && !r.running && !r.data.file.frames.empty())
    {
        replay_start(r, app.ifaces, app.tasks); // --replay FILE --measure: the measurement started first
    }
    return true;
}

void replay_start(Replay& r, std::deque<Iface>& ifaces, Tasks& tasks)
{
    replay_stop(r);
    r.plan = replay_plan(r.data.file, r.data.rows, r.mapping);
    r.position = 0;
    r.running = true;
    r.player = std::jthread(replay_run, std::ref(r), std::ref(ifaces), std::ref(tasks),
                            std::clamp(static_cast<double>(r.speed), 0.1, 10.0), r.loop);
}

void replay_stop(Replay& r)
{
    r.player = {}; // request_stop + join
    r.running = false;
}

void draw_replay(App& app, const WorkspaceTab& tab, Replay& r)
{
    // Autoplay follows the measurement, whether or not the window is shown.
    if (app.measuring != r.was_measuring)
    {
        r.was_measuring = app.measuring;
        if (r.autoplay && app.measuring && !r.running && !r.data.file.frames.empty())
        {
            replay_start(r, app.ifaces, app.tasks);
        }
        else if (r.autoplay && !app.measuring && r.running)
        {
            replay_stop(r);
            r.position = 0;
        }
    }
    if (!r.running && r.player.joinable())
    {
        r.player.join(); // finished on its own
    }
    replay_load_poll(app, r);
    if (!r.open && r.loader.joinable())
    {
        replay_load_cancel(r); // window closed while diving
        r.data.info.clear();
    }
    const WorkspaceTab* current = workspace_current(app.workspace);
    if (!r.open || current == nullptr || current->uid != tab.uid)
    {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(workspace_window_name(tab, "Replay").c_str(), &r.open))
    {
        const bool running = r.running;
        ImGui::BeginDisabled(running);
        if (icon_text_button("Load", Icon::DocumentOpen))
        {
            file_dialog_open(r.load_dialog, FileDialogMode::Open, "Load Trace File", r.data.path, trace_read_filters);
        }
        for (const auto& path : file_dialog_draw(r.load_dialog))
        {
            replay_load(app, r, path);
        }
        same_line_or_wrap(icon_text_button_width("Play"));
        ImGui::BeginDisabled(r.data.file.frames.empty());
        if (icon_text_button("Play", Icon::PlaybackStart))
        {
            replay_start(r, app.ifaces, app.tasks);
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        same_line_or_wrap(icon_text_button_width("Stop"));
        ImGui::BeginDisabled(!running);
        if (icon_text_button("Stop", Icon::PlaybackStop))
        {
            replay_stop(r);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("Autoplay", &r.autoplay);
        ImGui::SetItemTooltip("Automatically start/stop replay with measurement");
        ImGui::SameLine();
        ImGui::BeginDisabled(running);
        ImGui::Checkbox("Loop", &r.loop);
        ImGui::SetItemTooltip("Restart replay after all messages were sent");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        ImGui::SliderFloat("Speed", &r.speed, 0.1f, 10.0f, "%.1fx", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Playback speed multiplier");
        ImGui::EndDisabled();

        ImGui::TextDisabled("%s", r.data.path.empty() ? "No trace file loaded" : r.data.path.c_str());
        if (r.loader.joinable())
        {
            const float f = r.load_fraction;
            draw_depth_gauge(f, ImVec2(-FLT_MIN, 0.0f), nullptr, true);
            ImGui::SetItemTooltip("Diving... %.0f %% loaded", f * 100.0f);
        }
        else
        {
            const std::size_t total = running || r.position > 0 ? r.plan.size() : r.data.file.frames.size();
            const std::size_t pos = r.position;
            draw_depth_gauge(total == 0 ? 0.0f : static_cast<float>(pos) / static_cast<float>(total),
                             ImVec2(-FLT_MIN, 0.0f), r.data.file.frames.size() > 1 ? &r.data.gaps : nullptr, running);
            ImGui::SetItemTooltip("%s", std::format("{} / {} messages", pos, total).c_str());
        }
        ImGui::TextUnformatted(r.data.info.empty() ? "Trace file info..." : r.data.info.c_str());

        ImGui::BeginDisabled(running); // ponytail: filters and mapping are snapshotted at Play; make them live if editing mid-playback is wanted
        const bool select_all = ImGui::Button("Select All");
        ImGui::SameLine();
        if (ImGui::Button("Deselect All") || select_all)
        {
            for (ReplayIdRow& row : r.data.rows)
            {
                row.rx_on = select_all && row.has_rx;
                row.tx_on = select_all && row.has_tx;
            }
        }
        draw_filter_table(app, r);
        ImGui::EndDisabled();
    }
    ImGui::End();
}
