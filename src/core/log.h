/*

  Copyright (c) 2016 Hubert Denkmair <hubert@denkmair.de>
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

// Application log: a fixed-size ring buffer behind a mutex, callable from any thread.
// Usage: log_info(std::format("opened {}", name));
// The Log window locks log_state().mutex and walks the entries with log_entry_at().

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

enum class LogLevel : uint8_t
{
    Debug,
    Info,
    Warning,
    Error,
};

struct LogEntry
{
    std::chrono::system_clock::time_point time;
    LogLevel level = LogLevel::Info;
    std::string text;
};

struct LogState
{
    static constexpr size_t capacity = 5000;

    std::mutex mutex;
    std::vector<LogEntry> entries;   // ring, grows to capacity then wraps at head
    size_t head = 0;                 // oldest entry once the ring is full
    uint64_t total = 0;              // entries ever logged; lets the UI detect new ones
};

[[nodiscard]] LogState& log_state();

// i-th oldest entry, 0 <= i < entries.size(). Caller holds state.mutex.
[[nodiscard]] const LogEntry& log_entry_at(const LogState& state, size_t i);

void log_clear();
[[nodiscard]] std::string_view log_level_name(LogLevel level);

void log_msg(LogLevel level, std::string msg);
inline void log_debug(std::string msg)    { log_msg(LogLevel::Debug, std::move(msg)); }
inline void log_info(std::string msg)     { log_msg(LogLevel::Info, std::move(msg)); }
inline void log_warning(std::string msg)  { log_msg(LogLevel::Warning, std::move(msg)); }
inline void log_error(std::string msg)    { log_msg(LogLevel::Error, std::move(msg)); }
