/*

  Copyright (c) 2016 Hubert Denkmair <hubert@denkmair.de>
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

#include "log.h"

#include <utility>

LogState& log_state()
{
    static LogState state;
    return state;
}

const LogEntry& log_entry_at(const LogState& state, size_t i)
{
    return state.entries[(state.head + i) % state.entries.size()];
}

void log_clear()
{
    auto& s = log_state();
    std::scoped_lock lock(s.mutex);
    s.entries.clear();
    s.head = 0;
}

std::string_view log_level_name(LogLevel level)
{
    switch (level)
    {
        case LogLevel::Debug:    return "debug";
        case LogLevel::Info:     return "info";
        case LogLevel::Warning:  return "warning";
        case LogLevel::Error:    return "error";
    }
    return "";
}

void log_msg(LogLevel level, std::string msg)
{
    LogEntry entry{ .time = std::chrono::system_clock::now(), .level = level, .text = std::move(msg) };

    auto& s = log_state();
    std::scoped_lock lock(s.mutex);
    if (s.entries.size() < LogState::capacity)
    {
        s.entries.push_back(std::move(entry));
    }
    else
    {
        s.entries[s.head] = std::move(entry);
        s.head = (s.head + 1) % LogState::capacity;
    }
    ++s.total;
}
