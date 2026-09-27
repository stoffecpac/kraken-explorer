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

// Embedded Python scripting (the old PythonEngine): one interpreter, one script at a time
// on its own thread, exposed to scripts as the `kraken` module. Everything that touches
// App runs on the main thread through tasks_run_on_main_blocking with the GIL released;
// the main thread itself never holds the GIL after start-up. No Python headers here:
// app.h includes this file and is compiled by targets without the Python include path.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/bus_message.h"
#include "core/setup.h"

struct App;

// A run of console text in one colour (stdout or stderr of the script).
struct PyConsoleRun
{
    std::string text;
    bool error = false;
};

struct PyRxFilter
{
    uint32_t id = 0;
    uint32_t mask = 0;
    std::optional<bool> extended;
    std::optional<uint16_t> iface; // unset: every interface
    bool active = false;
};

// Not movable (mutexes, threads): keep it by value in App.
struct PyState
{
    void* main_thread_state = nullptr; // PyThreadState* the main thread parked with PyEval_SaveThread
    std::string init_error;            // set when the interpreter failed to start
    std::atomic<bool> running{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> echo_tx{false};  // kraken.enable_tx_echo: TX echoes reach receive()

    std::mutex mutex; // guards rx_queue, filter, input_queue, console
    std::condition_variable rx_cv;
    std::deque<BusMessage> rx_queue;   // fed by python_rx_consumer (RX threads), drained by receive()
    PyRxFilter filter;
    std::atomic<uint64_t> rx_dropped{0}; // frames lost to a full rx_queue this run
    std::condition_variable input_cv;
    std::deque<std::string> input_queue; // lines for the script's sys.stdin
    std::vector<PyConsoleRun> console;   // script output; runs of the same colour are merged
    std::size_t console_bytes = 0;
    uint64_t console_total = 0;          // bumped per append, lets the UI auto-scroll

    // Worker-side snapshot of the setup (shares the databases) and the interface names,
    // refreshed through the main thread at most once a second. Read with the GIL held only.
    Setup setup;
    std::vector<std::string> iface_names;
    std::chrono::steady_clock::time_point snapshot_time{};

    std::mutex periodic_mutex;
    std::map<int, std::jthread> periodic; // kraken.send_periodic tasks, keyed by handle
    int next_periodic = 0;

    std::jthread worker; // last: joined before the members it uses go away
};

// Starts `code` on the worker thread; initialises the interpreter on first use (main
// thread). Clears the console. Returns false, with the reason on the console, when a
// script is still running or Python failed to start.
// `name` is the file name tracebacks show.
bool python_run(App& app, PyState& s, std::string code, std::string name = "<script>");

// Asks the running script to stop (KeyboardInterrupt at its next Python line, receive()
// and input() return at once) and returns without waiting.
void python_stop(PyState& s);

// Main thread, every frame: joins the worker once the script has finished.
void python_poll(PyState& s);

// Main thread, at exit: stops the script, keeps draining main-thread tasks until it has
// ended (time.sleep() is interrupted; another blocking C call delays this), finalises Python.
void python_shutdown(App& app, PyState& s);

// A line typed into the console; becomes the next sys.stdin line of the script.
void python_input(PyState& s, std::string line);

// App::rx_consumers entry (user = &PyState): TX-echo and filter gate, then queue for receive().
void python_rx_consumer(void* user, const BusMessage& msg);

// Appends script output; any thread.
void python_console_append(PyState& s, std::string text, bool error);
void python_console_clear(PyState& s);
