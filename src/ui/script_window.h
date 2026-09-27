#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "ui/file_dialog.h"

struct App;
struct WorkspaceTab;

// The docked "Python Script" window (old ScriptWindow): editor and console side by side,
// a stdin line below, Run / Stop / AutoRun / Load / Save / Clear above. One state for
// every tab, since there is one interpreter and one script at a time.
struct ScriptWindowState
{
    std::string code;
    std::string file_path;                          // empty: nothing loaded
    std::filesystem::file_time_type loaded_time{};  // Run reloads the file when it changed since
    bool autorun = false;                           // start the script with the measurement
    bool was_measuring = false;                     // to see the measurement start/stop edges
    std::string input;                              // the stdin line being typed
    uint64_t console_seen = 0;                      // PyState::console_total last drawn: auto-scroll
    FileDialog file_dialog;                         // Load / Save
};

void draw_script_window(App& app, ScriptWindowState& s, const WorkspaceTab& tab);

// Loads a script file into the editor. Logs and returns false when it cannot be read.
bool script_window_load_file(ScriptWindowState& s, const std::string& path);

// Run: reloads a modified file, then starts the script.
void script_window_run(App& app, ScriptWindowState& s);
