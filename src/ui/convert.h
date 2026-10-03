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

// Convert (toolbar button, File > Convert...): a trace file to another trace format, or a CAN database to another database
// format. Logs go through the frame cache (a log opened before converts at once, a new one is
// parsed once) and are written on a worker thread, decoded and written in pieces, so a 12 GB log
// needs no more RAM than its cache.

#pragma once

#include <atomic>
#include <cstdint>
#include <future>
#include <stop_token>
#include <string>
#include <thread>

#include "ui/file_dialog.h"

struct App;

struct ConvertState
{
    bool open = false;
    // Log
    std::string log_in;
    std::string log_out;
    int log_format = 1; // index in trace_file_formats (VectorAsc)
    FileDialog log_in_dialog;
    FileDialog log_out_dialog;
    std::atomic<float> read_fraction{0.0f};  // parsing a log without a cache
    std::atomic<uint64_t> read_frames{0};
    std::atomic<uint64_t> written{0};        // frames written
    std::atomic<uint64_t> total{0};          // frames to write, 0 while still reading
    std::future<std::string> log_result;     // the worker's last line ("Wrote ...", "Error: ...")
    std::string log_status;
    // Database
    std::string db_in;
    std::string db_out;
    int db_format = 0; // index in convert_db_formats
    FileDialog db_in_dialog;
    FileDialog db_out_dialog;
    std::string db_status;
    std::jthread worker; // last: joins before the fields it writes go away
};

// Converts src to dst in `format` through src's frame cache (built when missing). Progress in
// s.read_* while parsing, s.written / s.total while writing; a stop request deletes the partial
// output. Returns the status line. Blocking (the window runs it on s.worker).
[[nodiscard]] std::string convert_log(const std::string& src, const std::string& dst, int format_index, ConvertState& s,
                                      const std::stop_token& stop = {});
// Reads a .dbc / .sym / .dbf and writes it as .dbc or .dbf (by dst's extension). Returns the status line.
[[nodiscard]] std::string convert_database(const std::string& src, const std::string& dst);

// The "Convert" window, opened by the toolbar button (right of DBC Editor) or File > Convert...;
// a conversion keeps running while it is closed.
void draw_convert(App& app, ConvertState& s);
