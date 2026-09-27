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

// Writes a fixed set of frames in every export format, plus TraceRecorder output
// split into parts, so validate.py can read everything back with independent
// readers (python-can, scapy, asammdf).
//
// The frame definitions are duplicated in validate.py on purpose: expectations
// there must not be derived from anything cangaroo writes.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "core/bus_message.h"
#include "core/trace_file_writer.h"
#include "core/trace_recorder.h"

namespace
{

namespace fs = std::filesystem;

constexpr int64_t base_us = 1757000000000000LL;
constexpr int bulk_count = 30000;

std::string iface_name(uint16_t iface)
{
    return iface == 1 ? "vcan0" : "vcan1";
}

BusMessage make(int64_t timestamp_us, uint16_t iface, uint32_t id, uint16_t flags, std::vector<uint8_t> data)
{
    BusMessage m{.id = id, .flags = flags, .iface = iface, .ts_ns = timestamp_us * 1000};
    set_length(m, static_cast<uint8_t>(data.size()));
    std::copy(data.begin(), data.end(), m.data.begin());
    return m;
}

// Keep in sync with SAMPLE_FRAMES in validate.py.
std::vector<BusMessage> sample_frames()
{
    using namespace bus_flag;
    std::vector<BusMessage> frames{
        make(base_us + 0, 1, 0x123, 0, {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}),
        make(base_us + 10000, 1, 0x18DAF110, extended | tx, {0xAA, 0xBB, 0xCC}),
        make(base_us + 20000, 2, 0x7DF, rtr, std::vector<uint8_t>(4, 0)),
        make(base_us + 30000, 2, 0x456, fd | brs, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}),
        make(base_us + 40000, 1, 0x1ABC0001, extended | fd | tx, std::vector<uint8_t>(64, 0xA5)),
        make(base_us + 50000, 1, 0x100, 0, {}),
        make(base_us + 60000, 2, 0, 0, {}),
    };
    frames.back().errors = bus_error::generic;
    return frames;
}

// Keep in sync with bulk_frame() in validate.py.
BusMessage bulk_frame(int i)
{
    std::vector<uint8_t> data;
    for (int k = 0; k < 8; ++k)
    {
        data.push_back(static_cast<uint8_t>((i * 31 + k * 7) & 0xFF));
    }
    return make(base_us + i * 1000LL, static_cast<uint16_t>(1 + i % 2), static_cast<uint32_t>(0x100 + i % 0x700),
                i % 3 != 0 ? 0 : bus_flag::tx, data);
}

bool record(const fs::path& folder, TraceFileFormat format)
{
    Recorder r;
    r.iface_name = iface_name;
    r.config = RecordingConfig{
        .folder = folder.string(),
        .file_name_pattern = "part_{index}",
        .format = format,
        .split_size_mb = 1,
        .stay_armed = false,
    };
    recorder_set_armed(r, true, false);
    recorder_measurement_starting(r);
    if (!r.recording)
    {
        return false;
    }
    for (int i = 0; i < bulk_count; ++i)
    {
        recorder_enqueue(r, bulk_frame(i));
    }
    recorder_measurement_stopped(r);
    return r.frames_written == static_cast<uint64_t>(bulk_count);
}

}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: trace_format_samples <output dir>\n");
        return 2;
    }

    const fs::path out = argv[1];
    std::error_code ec;
    fs::create_directories(out, ec);
    if (!fs::is_directory(out))
    {
        std::fprintf(stderr, "cannot create %s\n", argv[1]);
        return 1;
    }

    const std::vector<BusMessage> frames = sample_frames();

    for (const TraceFileFormat format : { TraceFileFormat::CanDump, TraceFileFormat::VectorAsc,
                                          TraceFileFormat::VectorMdf, TraceFileFormat::Pcap,
                                          TraceFileFormat::PcapNg, TraceFileFormat::Trc })
    {
        const std::string extension(trace_format_extension(format));
        std::ofstream file(out / ("export." + extension), std::ios::binary);
        std::ofstream empty_file(out / ("empty." + extension), std::ios::binary);
        if (!file || !empty_file)
        {
            std::fprintf(stderr, "cannot write %s samples\n", extension.c_str());
            return 1;
        }
        write_trace_file(file, format, frames, iface_name);
        write_trace_file(empty_file, format, {}, iface_name);
        if (!file.flush() || !empty_file.flush())
        {
            std::fprintf(stderr, "cannot write %s samples\n", extension.c_str());
            return 1;
        }
    }

    for (const TraceFileFormat format : { TraceFileFormat::VectorAsc, TraceFileFormat::CanDump,
                                          TraceFileFormat::PcapNg, TraceFileFormat::Trc })
    {
        const std::string name(trace_format_name(format));
        const fs::path folder = out / ("recorder_" + name);
        fs::remove_all(folder, ec);
        if (!record(folder, format))
        {
            std::fprintf(stderr, "recording %s failed\n", name.c_str());
            return 1;
        }
    }
    return 0;
}
