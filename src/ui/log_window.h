#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "core/log.h"
#include "ui/file_dialog.h"

struct App;
struct WorkspaceTab;

// Level buttons of the Log window: Error also covers Critical and Fatal.
enum class LogFilterLevel : uint8_t
{
    Debug,
    Info,
    Warning,
    Error,
};
inline constexpr size_t log_filter_levels = 4;

// The docked "Log" window (old LogWindow): level toggles with counts, a fuzzy search, Time /
// Level / Text of the lines that pass, Clear, Export. Lines are addressed by their absolute
// index (0 = first line ever logged; the ring holds [total - entries.size(), total)).
struct LogWindowState
{
    uint64_t seen_total = 0; // log_state().total last drawn; a change scrolls to the bottom if it was there
    FileDialog export_dialog;
    VimNav vim; // j/k/gg/G/Ctrl+d.. scroll the lines, h/l scroll sideways, / focuses the search
    std::string query;                              // fuzzy_score pattern, empty = all lines
    std::array<bool, log_filter_levels> show{true, true, true, true};
    std::array<size_t, log_filter_levels> counts{}; // lines per level in the log (ignoring the filter)
    std::deque<uint64_t> rows;                      // absolute index of each shown line, oldest first
    uint64_t done = 0;                              // log.total up to which lines are counted and matched
    uint64_t base = 0;                              // oldest absolute index at the last update
    std::string applied_query;                      // query / show that rows was built with
    std::array<bool, log_filter_levels> applied_show{true, true, true, true};
    std::vector<uint8_t> level_ring;                // LogFilterLevel of line a at a % LogState::capacity
    std::vector<int> positions;                     // reused for highlighting
    bool focus_search = false;                      // '/' pressed: focus the search box next frame
};

// Brings counts and rows up to date with log: only lines added since the last call are matched,
// all lines only when query or show changed. Caller holds log.mutex.
void log_window_update(LogWindowState& s, const LogState& log);

void draw_log_window(LogWindowState& s, const WorkspaceTab& tab);
