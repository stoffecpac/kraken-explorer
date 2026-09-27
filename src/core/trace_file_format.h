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

// Trace export formats and the mapping between them, file extensions and the
// format names used by the Python API. Kept free of the trace so the mapping can
// be tested on its own, and shared so the GUI save dialog and the scripting API
// cannot drift apart.

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

enum class TraceFileFormat
{
    CanDump,    // Linux candump text
    VectorAsc,  // Vector ASCII
    VectorMdf,  // Vector MDF4
    Pcap,       // pcap, LINKTYPE_CAN_SOCKETCAN
    PcapNg,     // pcapng, LINKTYPE_CAN_SOCKETCAN
    Trc         // PEAK PCAN trace, version 2.1
};

inline constexpr std::array<TraceFileFormat, 6> trace_file_formats = {
    TraceFileFormat::CanDump, TraceFileFormat::VectorAsc, TraceFileFormat::VectorMdf,
    TraceFileFormat::Pcap, TraceFileFormat::PcapNg, TraceFileFormat::Trc,
};

// File extension for a format, without the leading dot.
[[nodiscard]] constexpr std::string_view trace_format_extension(TraceFileFormat format) noexcept
{
    switch (format)
    {
        case TraceFileFormat::CanDump:   return "candump";
        case TraceFileFormat::VectorAsc: return "asc";
        case TraceFileFormat::VectorMdf: return "mf4";
        case TraceFileFormat::Pcap:      return "pcap";
        case TraceFileFormat::PcapNg:    return "pcapng";
        case TraceFileFormat::Trc:       return "trc";
    }
    return {};
}

// Format name accepted by the scripting API.
[[nodiscard]] constexpr std::string_view trace_format_name(TraceFileFormat format) noexcept
{
    return format == TraceFileFormat::VectorMdf ? "mdf" : trace_format_extension(format);
}

// Resolves an explicit format name. Accepts the canonical names above plus a few
// obvious aliases, ignoring case and surrounding whitespace. Returns nullopt for
// anything else -- callers report the error rather than guessing.
[[nodiscard]] inline std::optional<TraceFileFormat> trace_format_from_name(std::string_view name)
{
    constexpr std::string_view space = " \t\n\r\f\v";
    const auto first = name.find_first_not_of(space);
    if (first == std::string_view::npos)
    {
        return std::nullopt;
    }
    // ponytail: trim duplicates core/text.h; including it here clashes with ui/replay.cpp's iequals until T70.
    std::string key(name.substr(first, name.find_last_not_of(space) - first + 1));
    std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (key == "candump" || key == "log")                  { return TraceFileFormat::CanDump; }
    if (key == "asc" || key == "vector_asc")               { return TraceFileFormat::VectorAsc; }
    if (key == "mdf" || key == "mf4" || key == "mdf4")     { return TraceFileFormat::VectorMdf; }
    if (key == "pcap")                                     { return TraceFileFormat::Pcap; }
    if (key == "pcapng")                                   { return TraceFileFormat::PcapNg; }
    if (key == "trc")                                      { return TraceFileFormat::Trc; }
    return std::nullopt;
}

// Infers the format from a file name's extension (text after the last dot).
[[nodiscard]] inline std::optional<TraceFileFormat> trace_format_from_path(std::string_view path)
{
    const auto dot = path.rfind('.');
    return dot == std::string_view::npos ? std::nullopt : trace_format_from_name(path.substr(dot + 1));
}
