#pragma once

#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "core/python_engine.h"
#include "core/setup.h"
#include "core/tasks.h"
#include "core/trace.h"
#include "core/trace_recorder.h"
#include "drivers/driver.h"
#include "ui/can_status.h"
#include "ui/conditional_logging.h"
#include "ui/dbc_editor.h"
#include "ui/lin_control.h"
#include "ui/log_window.h"
#include "ui/gateway.h"
#include "ui/instrument_panel.h"
#include "ui/main_menu.h"
#include "ui/recording_dialog.h"
#include "ui/file_dialog.h"
#include "ui/replay.h"
#include "ui/script_window.h"
#include "ui/settings.h"
#include "ui/settings_dialog.h"
#include "ui/setup_dialog.h"
#include "ui/status_bar.h"
#include "ui/theme.h"
#include "ui/tx_generator.h"
#include "ui/vim_nav.h"
#include "ui/trace_window.h"
#include "ui/workspace_tabs.h"

// All application state. Grows as the port proceeds (trace, setup, interfaces, ...).
struct App
{
    bool quit = false;
    bool measuring = false; // measurement running; set by measurement start/stop (T16)
    MainMenu menu;          // menu/control-bar commands, recent files, record toggle
    Settings settings;      // ini-backed settings + current workspace file
    WorkspaceTabs workspace; // bottom tabs, one dockspace each
    ThemeFonts fonts; // fonts.mono for hex/data columns
    Tasks tasks;      // work posted from other threads; tasks.wake doubles as the RX wake-up
    Trace trace;
    std::unordered_map<unsigned, TraceWindowState> trace_windows; // per workspace tab uid
    Recorder recorder; // follows menu.record_armed; not movable, so App stays put
    std::chrono::steady_clock::time_point recorder_drained{};
    LogWindowState log_window;       // shared by every tab's Log window
    CanStatusState can_status;       // counters polled while measuring
    VimNav vim_windows;              // Ctrl+w window moves (ui/vim_nav)
    RecordingDialogState recording_dialog;
    SettingsDialogState settings_dialog;
    StatusBarState status_bar;       // bottom status line + menu bar connection corner
    Setup setup;
    SetupDialogState setup_dialog; // Measurement > Setup; edits a copy of setup
    std::deque<Iface> ifaces;              // every enumerated channel, index = BusMessage::iface
    std::vector<RxConsumer> rx_consumers;  // run on the RX threads, changed only while they are stopped
    std::vector<BusMessage> rx_scratch;
    std::map<unsigned, TxGenerator> tx_generators; // key: WorkspaceTab::uid; after ifaces (its thread sends on them)
    Gateway gateway;                         // an RX consumer; forwards between ifaces
    std::map<unsigned, Replay> replays;      // key: WorkspaceTab::uid; after ifaces (its thread sends on them)
    FileDialog trace_file_dialog;            // Trace > Save Trace to file / Export full trace / Import full trace
    // Graph > Export to PNG: main.cpp reads this rectangle (ImGui screen coordinates, main
    // viewport only) from the framebuffer after the frame is rendered and writes the file.
    struct Screenshot
    {
        std::string path;
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    };
    std::optional<Screenshot> screenshot;
    std::map<unsigned, LinControl> lin_controls; // key: WorkspaceTab::uid
    std::map<unsigned, InstrumentPanel> instrument_panels; // key: WorkspaceTab::uid; after ifaces (sends on them)
    std::map<unsigned, DbcEditorState> dbc_editors;        // key: WorkspaceTab::uid; edits a copy of a DBC
    ConditionalLogging conditional_logging;  // fed from the trace every frame
    PyState python;                          // an RX consumer; one interpreter, one script at a time
    ScriptWindowState script;                // the "Python Script" window, shared by every tab
};

// Enumerates the drivers and, if the setup is empty, fills it with one network per interface.
void app_init_interfaces(App& app);
// Opens every enabled setup interface and starts its listener thread.
void app_measurement_start(App& app);
// Joins the listener threads and closes the interfaces. Call before glfwTerminate.
void app_measurement_stop(App& app);

// Work that must not run inside a frame (loading a workspace replaces the dock layout).
// Called right before ImGui::NewFrame().
void app_before_frame(App& app);

// Draws one frame of the UI. Called between ImGui::NewFrame() and ImGui::Render().
// Writes every frame of the trace store to path, format from its extension; errors go to the log.
void app_trace_save(App& app, const std::string& path);
void app_frame(App& app);
