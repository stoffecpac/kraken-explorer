#pragma once

// Replay View: loads a candump / ASC / pcap / pcapng trace and plays it back on the
// interfaces (or into the trace) with the original timing, on its own jthread.

#include <atomic>
#include <cstdint>
#include <deque>
#include <future>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/bus_message.h"
#include "core/stats.h"
#include "core/trace_file_format.h"
#include "ui/file_dialog.h"

struct App;
struct Iface;
struct Tasks;
struct WorkspaceTab;

inline constexpr uint32_t replay_error_id = 0xFFFFFFFF; // filter key of error frames
inline constexpr int replay_trace_only = -1;            // channel mapping: no interface

// A parsed trace file. frames[i].iface is an index into channels (e.g. "vcan0", "CH 1").
struct ReplayFile
{
    std::vector<BusMessage> frames;
    std::vector<std::string> channels;
};

// Optional hooks for parsing on a loader thread: fraction gets bytes parsed / data size now
// and then, and a stop request ends the parse early (the partial file is returned).
struct ReplayParseProgress
{
    std::stop_token stop;
    std::atomic<float>* fraction = nullptr;
};

// Parses a whole file's bytes; frames without a usable line are skipped. VectorMdf is not
// readable (as before the port) and gives an empty file.
[[nodiscard]] ReplayFile replay_parse(std::string_view data, TraceFileFormat format,
                                      const ReplayParseProgress& progress = {});

// One row of the filter tree: a (channel, id) pair seen in the file.
struct ReplayIdRow
{
    uint16_t channel = 0;
    uint32_t id = 0; // replay_error_id for error frames
    bool extended = false; // of the first frame with this id, for the DBC lookup
    int count = 0;
    bool has_rx = false;
    bool has_tx = false;
    bool rx_on = false; // replayed when the frame's direction is enabled
    bool tx_on = false;
};

// Rows sorted by (channel, id), all enabled.
[[nodiscard]] std::vector<ReplayIdRow> replay_id_rows(const ReplayFile& file);

// A frame to send at `at_ns` after the start (speed 1). iface = target interface, or the
// file channel when target == replay_trace_only.
struct ReplayStep
{
    BusMessage msg;
    int64_t at_ns = 0;
    int target = replay_trace_only;
};

// The enabled frames in order. mapping[channel] is an App::ifaces index or replay_trace_only;
// LIN and error frames always go to the trace only.
[[nodiscard]] std::vector<ReplayStep> replay_plan(const ReplayFile& file, const std::vector<ReplayIdRow>& rows,
                                                  const std::vector<int>& mapping);

// What the loader thread hands to the main thread (through Replay::loading).
struct ReplayLoaded
{
    std::string path; // empty on error
    std::string info;
    ReplayFile file;
    std::vector<ReplayIdRow> rows;
    std::vector<char> channel_lin;
    Stats gaps; // inter-frame time in seconds, computed once by the loader
};

// State of one Replay View. Not movable (thread, atomics).
struct Replay
{
    bool open = false;
    FileDialog load_dialog;
    ReplayLoaded data;        // the loaded file (data.info: name, count, duration or the load error)
    std::vector<int> mapping; // per file channel
    float speed = 1.0f;
    bool autoplay = false;
    bool loop = false;
    bool was_measuring = false;

    // Shared with the player thread; plan is only touched while it is not running.
    std::vector<ReplayStep> plan;
    std::atomic<std::size_t> position{0};
    std::atomic<bool> running{false};
    std::jthread player; // joined before the members it uses go away

    // Loader thread: reads + parses off the main thread. loader.joinable() == loading
    // (only the main thread touches the jthread object).
    std::atomic<float> load_fraction{0.0f}; // bytes parsed / file size, 1 when done
    std::future<ReplayLoaded> loading; // set once by the loader; never set when stopped
    std::jthread loader; // last: stopped and joined first
};

// Player thread body: sends plan[i] when due (start + at_ns / speed), steps that cannot be
// sent on their interface go to the trace through tasks. Loops if asked. Clears running.
void replay_run(std::stop_token stop, Replay& r, std::deque<Iface>& ifaces, Tasks& tasks, double speed, bool loop);

// Starts reading and parsing a file (format from the extension, ASC otherwise) on r.loader;
// a load already running is cancelled. r.load_fraction shows the progress. Returns at once.
void replay_load(App& app, Replay& r, const std::string& path);

// Main thread: if the loader has finished, takes its result, maps each channel to the
// interface of the same name and returns true (starts autoplay if measuring). Errors end up
// in r.data.info. draw_replay calls it every frame.
bool replay_load_poll(App& app, Replay& r);

// Cancels a running load (joins the loader) and discards its result.
void replay_load_cancel(Replay& r);

// Builds the plan and starts the player; replay_stop joins it.
void replay_start(Replay& r, std::deque<Iface>& ifaces, Tasks& tasks);
void replay_stop(Replay& r);

// Runs autoplay (start/stop with the measurement) and, while r.open and tab is the current
// workspace tab, draws its "Replay" window. Call for every tab that has a Replay.
void draw_replay(App& app, const WorkspaceTab& tab, Replay& r);
