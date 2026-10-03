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

#include "ui/convert.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <vector>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "app.h"
#include "core/text.h"
#include "core/trace_file_format.h"
#include "core/trace_file_writer.h"
#include "db/dbc/dbc_writer.h"
#include "db/dbf/dbf.h"
#include "db/sym/sym_parser.h"
#include "ui/frame_cache.h"
#include "ui/main_menu.h"
#include "ui/theme.h"

namespace
{

struct DbFormat
{
    const char* label;
    const char* extension;
};
constexpr DbFormat convert_db_formats[] = {{"DBC (*.dbc)", ".dbc"}, {"BUSMASTER DBF (*.dbf)", ".dbf"}};
const std::vector<FileFilter> db_read_filters = {{"CAN Databases", "*.dbc *.dbf *.sym"}, {"All Files", "*"}};

const char* trace_format_label(TraceFileFormat f)
{
    switch (f)
    {
        case TraceFileFormat::CanDump:   return "Linux candump (*.candump)";
        case TraceFileFormat::VectorAsc: return "Vector ASC (*.asc)";
        case TraceFileFormat::VectorMdf: return "ASAM MDF4 (*.mf4)";
        case TraceFileFormat::Pcap:      return "PCAP (*.pcap)";
        case TraceFileFormat::PcapNg:    return "PCAPng (*.pcapng)";
        case TraceFileFormat::Trc:       return "PEAK PCAN trace (*.trc)";
        case TraceFileFormat::Blf:       return "Vector BLF (*.blf)";
    }
    return "";
}

// in with its extension replaced (the output path follows the input and the format until edited).
std::string with_extension(const std::string& in, std::string_view ext)
{
    return in.empty() ? std::string() : std::filesystem::path(in).replace_extension(ext).string();
}

// A path field with a Browse button that opens d.
void path_field(const char* id, std::string& path, FileDialog& d, FileDialogMode mode, const char* title,
                const std::vector<FileFilter>& filters)
{
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Browse...").x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputText("##path", &path);
    ImGui::SameLine();
    if (ImGui::Button("Browse..."))
    {
        file_dialog_open(d, mode, title, path, filters);
    }
    for (const std::string& picked : file_dialog_draw(d))
    {
        path = picked;
    }
    ImGui::PopID();
}

void status_line(const std::string& status)
{
    if (status.empty())
    {
        return;
    }
    if (status.starts_with("Error") || status.starts_with("Cancelled"))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme_text(ThemeText::error)));
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::PopStyleColor();
        return;
    }
    ImGui::TextWrapped("%s", status.c_str());
}

void draw_log_tab(ConvertState& s)
{
    const bool busy = s.worker.joinable() && s.log_result.valid();
    ImGui::BeginDisabled(busy);
    ImGui::TextUnformatted("Input");
    const std::string before = s.log_in;
    path_field("in", s.log_in, s.log_in_dialog, FileDialogMode::Open, "Log to Convert", trace_read_filters);
    const TraceFileFormat format = trace_file_formats[static_cast<std::size_t>(s.log_format)];
    if (s.log_in != before && !s.log_in.empty())
    {
        s.log_out = with_extension(s.log_in, std::string(".") + std::string(trace_format_extension(format)));
    }
    ImGui::TextUnformatted("Output format");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##format", trace_format_label(format)))
    {
        for (int i = 0; i < static_cast<int>(trace_file_formats.size()); ++i)
        {
            if (ImGui::Selectable(trace_format_label(trace_file_formats[static_cast<std::size_t>(i)]), i == s.log_format))
            {
                s.log_format = i;
                s.log_out = with_extension(s.log_out.empty() ? s.log_in : s.log_out,
                                           std::string(".") + std::string(trace_format_extension(trace_file_formats[static_cast<std::size_t>(i)])));
            }
        }
        ImGui::EndCombo();
    }
    ImGui::TextUnformatted("Output");
    path_field("out", s.log_out, s.log_out_dialog, FileDialogMode::Save, "Converted Log",
               {{trace_format_label(format), std::string("*.") + std::string(trace_format_extension(format))}, {"All Files", "*"}});
    ImGui::EndDisabled();

    if (!busy)
    {
        ImGui::BeginDisabled(s.log_in.empty() || s.log_out.empty());
        if (ImGui::Button("Convert"))
        {
            s.log_status.clear();
            std::promise<std::string> done;
            s.log_result = done.get_future();
            s.worker = std::jthread([&s, in = s.log_in, out = s.log_out, f = s.log_format, done = std::move(done)](const std::stop_token& stop) mutable
            {
                done.set_value(convert_log(in, out, f, s, stop));
            });
        }
        ImGui::EndDisabled();
    }
    else
    {
        const uint64_t total = s.total.load();
        const float fraction = total == 0 ? 0.5f * s.read_fraction.load() : 0.5f + 0.5f * static_cast<float>(s.written.load()) / static_cast<float>(total);
        std::string done_count;
        append_grouped(done_count, total == 0 ? s.read_frames.load() : s.written.load());
        const std::string label = std::format("{} {} frames", total == 0 ? "Reading" : "Writing", done_count);
        ImGui::ProgressBar(fraction, ImVec2(-ImGui::CalcTextSize("Cancel").x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x, 0.0f),
                           label.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            s.worker.request_stop();
        }
        if (s.log_result.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            s.log_status = s.log_result.get();
            s.worker = {};
        }
    }
    status_line(s.log_status);
}

void draw_db_tab(ConvertState& s)
{
    ImGui::TextUnformatted("Input (DBC, DBF or SYM)");
    const std::string before = s.db_in;
    path_field("dbin", s.db_in, s.db_in_dialog, FileDialogMode::Open, "Database to Convert", db_read_filters);
    if (s.db_in != before && !s.db_in.empty())
    {
        // The other format by default: .dbc -> .dbf, anything else -> .dbc.
        s.db_format = std::filesystem::path(s.db_in).extension() == ".dbc" ? 1 : 0;
        s.db_out = with_extension(s.db_in, convert_db_formats[s.db_format].extension);
    }
    ImGui::TextUnformatted("Output format");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##dbformat", convert_db_formats[s.db_format].label))
    {
        for (int i = 0; i < static_cast<int>(std::size(convert_db_formats)); ++i)
        {
            if (ImGui::Selectable(convert_db_formats[i].label, i == s.db_format))
            {
                s.db_format = i;
                s.db_out = with_extension(s.db_out.empty() ? s.db_in : s.db_out, convert_db_formats[i].extension);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::TextUnformatted("Output");
    path_field("dbout", s.db_out, s.db_out_dialog, FileDialogMode::Save, "Converted Database",
               {{convert_db_formats[s.db_format].label, std::string("*") + convert_db_formats[s.db_format].extension}, {"All Files", "*"}});
    ImGui::BeginDisabled(s.db_in.empty() || s.db_out.empty());
    if (ImGui::Button("Convert##db"))
    {
        s.db_status = convert_database(s.db_in, s.db_out);
    }
    ImGui::EndDisabled();
    status_line(s.db_status);
}

} // namespace

std::string convert_log(const std::string& src, const std::string& dst, int format_index, ConvertState& s, const std::stop_token& stop)
{
    const auto t0 = std::chrono::steady_clock::now();
    s.read_fraction = 0.0f;
    s.read_frames = 0;
    s.written = 0;
    s.total = 0;
    std::error_code ec;
    if (std::filesystem::equivalent(src, dst, ec))
    {
        return "Error: the output is the input file";
    }
    const auto format = trace_file_formats[static_cast<std::size_t>(format_index)];
    const std::filesystem::path cache_path = frame_cache_path(src);
    auto cache = frame_cache_open(src, cache_path);
    if (!cache)
    {
        cache = frame_cache_build(src, cache_path, trace_format_from_path(src).value_or(TraceFileFormat::VectorAsc),
                                  {.stop = stop, .fraction = &s.read_fraction, .frames = &s.read_frames});
    }
    if (stop.stop_requested())
    {
        if (cache)
        {
            frame_cache_close(*cache);
        }
        return "Cancelled";
    }
    if (!cache)
    {
        return "Error: " + cache.error();
    }
    s.total = cache->recs.size();
    bool ok = false;
    {
        std::vector<char> buffer(std::size_t{4} << 20);
        std::ofstream out;
        out.rdbuf()->pubsetbuf(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        out.open(dst, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            frame_cache_close(*cache);
            return "Error: cannot write " + dst;
        }
        const std::vector<std::string>& names = cache->channels;
        ok = write_trace_file(out, format, cache->recs, cache->overflow,
                              [&names](uint16_t i) { return i < names.size() ? names[i] : std::to_string(i); }, &s.written, stop);
        out.close();
        ok = ok && !out.fail();
    }
    const uint64_t frames = cache->recs.size();
    frame_cache_close(*cache);
    if (!ok)
    {
        std::filesystem::remove(dst, ec);
        return stop.stop_requested() ? "Cancelled" : "Error: writing " + dst + " failed (disk full?)";
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::string count;
    append_grouped(count, frames);
    const double mb = static_cast<double>(std::filesystem::file_size(dst, ec)) / (1 << 20);
    return std::format("Wrote {} frames to {} ({:.0f} MB) in {}{}", count, std::filesystem::path(dst).filename().string(), mb,
                       format_duration(secs, secs < 10.0 ? 2 : 0), secs < 60.0 ? " s" : "");
}

std::string convert_database(const std::string& src, const std::string& dst)
{
    CanDb db;
    if (!can_db_parse_file(src, db))
    {
        return "Error: cannot read " + src + " (details in the Log window)";
    }
    std::string error;
    std::error_code ec;
    if (std::filesystem::equivalent(src, dst, ec))
    {
        return "Error: the output is the input file";
    }
    const bool dbf = std::filesystem::path(dst).extension() == ".dbf";
    if (!(dbf ? dbf_write_file(db, dst, &error) : dbc_write_file(db, dst, &error)))
    {
        return "Error: " + error;
    }
    std::size_t signals = 0;
    for (const auto& [id, m] : db.messages)
    {
        signals += m.signals.size();
    }
    return std::format("Wrote {} messages, {} signals to {}", db.messages.size(), signals, std::filesystem::path(dst).filename().string());
}

void draw_convert(App& app, ConvertState& s)
{
    if (menu_take(app.menu, Command::Convert))
    {
        s.open = true;
        ImGui::SetWindowFocus("Convert");
    }
    if (!s.open)
    {
        return;
    }
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 36.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::Begin("Convert", &s.open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (ImGui::BeginTabBar("##convert"))
        {
            if (ImGui::BeginTabItem("Log"))
            {
                draw_log_tab(s);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Database"))
            {
                draw_db_tab(s);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}
