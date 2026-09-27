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

// LIN description database, loaded from an LDF file via db/ldf/ldf_parser.h.

#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct LinSignal
{
    std::string name;
    uint8_t bit_offset = 0;
    uint8_t bit_length = 0;
    std::string publisher;
    double factor = 1.0;
    double offset = 0.0;
    double min = 0.0;
    double max = 0.0;
    std::string unit;
    uint64_t init_value = 0;
    std::map<uint64_t, std::string> value_table;
};

struct LinFrame
{
    uint8_t id = 0;
    std::string name;
    std::string publisher;
    uint8_t length = 0;
    std::vector<LinSignal> signals;
};

struct LinDiagTiming
{
    uint16_t p2_min_ms = 25;
    uint16_t st_min_ms = 0;
    uint16_t n_as_ms = 1000;
    uint16_t n_cr_ms = 1000;
};

struct LinScheduleEntry
{
    uint8_t frame_id = 0;
    std::string frame_name;
    std::string publisher_name;
    uint8_t dlc = 0;
    uint8_t delay_ms = 0;
    bool is_master_publisher = false;
    bool is_sporadic = false;
};

struct LinScheduleTable
{
    std::string name;
    std::vector<LinScheduleEntry> entries;   // sporadic groups expanded, one entry per frame
};

struct LinDb
{
    std::string path;
    std::string protocol_version;
    double speed_bps = 19200.0;
    std::string master_node;
    std::vector<std::string> slave_nodes;
    double master_timebase_ms = 0.0;
    double master_jitter_ms = 0.0;
    std::map<uint8_t, LinFrame> frames;   // keyed by frame id
    std::vector<LinScheduleTable> schedule_tables;
    std::map<std::string, LinDiagTiming, std::less<>> diag_timings;
    std::map<std::string, uint8_t, std::less<>> node_nads;
    std::string last_error;
};

// Replaces db's contents on success; on failure only db.last_error changes.
[[nodiscard]] bool lin_db_load(LinDb& db, const std::string& path);

[[nodiscard]] const LinFrame* lin_db_find_frame(const LinDb& db, std::string_view name);
// Defaults for an unknown node.
// Node the diagnostic timings/NAD come from: the first slave for a master, else the configured slave.
[[nodiscard]] std::string lin_db_diag_node(const LinDb& db, bool master, std::string_view slave);
[[nodiscard]] LinDiagTiming lin_db_diag_timing(const LinDb& db, std::string_view node);
// 0 for an unknown node.
[[nodiscard]] uint8_t lin_db_node_nad(const LinDb& db, std::string_view node);

[[nodiscard]] const LinSignal* lin_frame_find_signal(const LinFrame& frame, std::string_view name);

// LIN signals are little-endian (LSB first); bits past data.size() read as 0.
[[nodiscard]] uint64_t lin_signal_extract_raw(const LinSignal& sig, std::span<const uint8_t> data) noexcept;
[[nodiscard]] double lin_signal_raw_to_physical(const LinSignal& sig, uint64_t raw) noexcept;
[[nodiscard]] double lin_signal_extract_physical(const LinSignal& sig, std::span<const uint8_t> data) noexcept;
// Empty when the value has no name.
[[nodiscard]] std::string_view lin_signal_value_name(const LinSignal& sig, uint64_t value);
