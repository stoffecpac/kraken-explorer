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

// Trace export format resolution.
//
// Both the GUI save dialog and kraken.save_trace() route through this mapping,
// so picking the wrong format here means silently writing a file whose contents
// do not match its extension. The ".pcap" / ".pcapng" pair is the trap: one is a
// suffix of the other, and an ordering mistake writes plain pcap into a .pcapng.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include "core/trace_file_format.h"

TEST_CASE("format from path")
{
    struct Row { const char* path; TraceFileFormat expected; };
    constexpr Row rows[] = {
        { "/tmp/run.candump", TraceFileFormat::CanDump },
        { "/tmp/run.log",     TraceFileFormat::CanDump },
        { "/tmp/run.asc",     TraceFileFormat::VectorAsc },
        { "/tmp/run.mf4",     TraceFileFormat::VectorMdf },
        { "/tmp/run.mdf",     TraceFileFormat::VectorMdf },
        { "/tmp/run.mdf4",    TraceFileFormat::VectorMdf },
        { "/tmp/run.pcap",    TraceFileFormat::Pcap },
        { "/tmp/run.pcapng",  TraceFileFormat::PcapNg },
        { "/tmp/run.trc",     TraceFileFormat::Trc },
        { "run.asc",          TraceFileFormat::VectorAsc },   // bare name
    };
    for (const Row& r : rows)
    {
        CAPTURE(r.path);
        CHECK(trace_format_from_path(r.path) == r.expected);
    }
}

TEST_CASE("format from path is case-insensitive")
{
    for (const char* path : { "/tmp/RUN.ASC", "/tmp/run.Asc", "/tmp/run.aSC" })
    {
        CAPTURE(path);
        CHECK(trace_format_from_path(path) == TraceFileFormat::VectorAsc);
    }
    CHECK(trace_format_from_path("/tmp/run.PCAPNG") == TraceFileFormat::PcapNg);
}

// "pcap" is a suffix of "pcapng"; a naive ends_with chain gets this wrong.
TEST_CASE("pcapng is not mistaken for pcap")
{
    CHECK(trace_format_from_path("/tmp/capture.pcapng") == TraceFileFormat::PcapNg);
    CHECK(trace_format_from_path("/tmp/capture.pcap") == TraceFileFormat::Pcap);

    // A name that merely contains "pcap" must key off the real extension.
    CHECK(trace_format_from_path("/tmp/pcap-notes.asc") == TraceFileFormat::VectorAsc);
    CHECK(trace_format_from_path("/tmp/my.pcapng.asc") == TraceFileFormat::VectorAsc);
}

// Callers report an error rather than guessing; the GUI applies its own ASC
// default on top of a nullopt, the scripting API raises.
TEST_CASE("format from path rejects unknown extensions")
{
    for (const char* path : { "/tmp/run", "/tmp/run.", "/tmp/run.txt", "/tmp/run.csv",
                              "", "." })
    {
        CAPTURE(path);
        CHECK_FALSE(trace_format_from_path(path).has_value());
    }
}

// The last dot decides, not the first -- a dotted directory must not be read as
// the extension.
TEST_CASE("format from path handles dots in directories")
{
    CHECK(trace_format_from_path("/home/u/my.traces/run.asc") == TraceFileFormat::VectorAsc);
    CHECK_FALSE(trace_format_from_path("/home/u/my.traces/run").has_value());
}

TEST_CASE("format from name")
{
    struct Row { const char* name; TraceFileFormat expected; };
    constexpr Row rows[] = {
        { "candump", TraceFileFormat::CanDump },
        { "asc",     TraceFileFormat::VectorAsc },
        { "mdf",     TraceFileFormat::VectorMdf },
        { "pcap",    TraceFileFormat::Pcap },
        { "pcapng",  TraceFileFormat::PcapNg },
        { "trc",     TraceFileFormat::Trc },
        // aliases
        { "log",        TraceFileFormat::CanDump },
        { "vector_asc", TraceFileFormat::VectorAsc },
        { "mf4",        TraceFileFormat::VectorMdf },
        { "mdf4",       TraceFileFormat::VectorMdf },
        // case and surrounding whitespace
        { "ASC",       TraceFileFormat::VectorAsc },
        { "PcapNg",    TraceFileFormat::PcapNg },
        { "  asc  ",   TraceFileFormat::VectorAsc },
        { "\tpcap\n",  TraceFileFormat::Pcap },
    };
    for (const Row& r : rows)
    {
        CAPTURE(r.name);
        CHECK(trace_format_from_name(r.name) == r.expected);
    }
}

TEST_CASE("format from name rejects unknown names")
{
    for (const char* name : { "", "  ", "csv", "vector", "asc2", "pcap ng" })
    {
        CAPTURE(name);
        CHECK_FALSE(trace_format_from_name(name).has_value());
    }
}

// Guards against a new enumerator being added without its name/extension case,
// which the switch statements would otherwise return an empty string for.
TEST_CASE("every format has a name and an extension")
{
    for (const TraceFileFormat format : trace_file_formats)
    {
        CHECK_FALSE(trace_format_name(format).empty());
        CHECK_FALSE(trace_format_extension(format).empty());
    }
}

// Every name the error messages advertise must actually resolve.
TEST_CASE("canonical names are all supported")
{
    for (const TraceFileFormat format : trace_file_formats)
    {
        const std::string_view name = trace_format_name(format);
        CAPTURE(name);
        CHECK(trace_format_from_name(name) == format);
    }
}

// A file named with a format's own extension must resolve back to that format.
TEST_CASE("extensions round-trip through format from path")
{
    for (const TraceFileFormat format : trace_file_formats)
    {
        const std::string path = "/tmp/trace." + std::string(trace_format_extension(format));
        CAPTURE(path);
        CHECK(trace_format_from_path(path) == format);
    }
}
