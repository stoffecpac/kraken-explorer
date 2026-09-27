#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app.h"
#include "core/log.h"
#include "core/python_engine.h"
#include "drivers/driver.h"

using namespace std::chrono_literals;

namespace
{

// Serves the script's main-thread calls until it has finished, as app_frame would.
void run_to_end(App& app)
{
    while (app.python.running)
    {
        tasks_drain(app.tasks, app);
        std::this_thread::sleep_for(2ms);
    }
    python_poll(app.python);
}

std::string console_text(PyState& py, bool errors_only = false)
{
    const std::lock_guard lock(py.mutex);
    std::string out;
    for (const PyConsoleRun& run : py.console)
    {
        if (!errors_only || run.error)
        {
            out += run.text;
        }
    }
    return out;
}

// Serves main-thread calls until the script has printed `text`.
void wait_for(App& app, const std::string& text)
{
    while (console_text(app.python).find(text) == std::string::npos)
    {
        tasks_drain(app.tasks, app);
        std::this_thread::sleep_for(2ms);
    }
}

} // namespace

TEST_CASE("embedded script: print, Message, receive() via the RX consumer, stop, errors")
{
    App app;
    PyState& py = app.python;

    REQUIRE(python_run(app, py, R"(
import kraken
print("size", kraken.trace_size())
m = kraken.Message(0x123)
m.set_data(b"\x01\x02")
print(repr(m), m.dlc, m.extended, m.is_rx)
for f in kraken.receive(timeout=5.0):
    print("rx", hex(f.id), f.get_data().hex(), f.interface_id)
print("ifaces", kraken.interfaces())
print("dbc", kraken.find_message("Nothing"), kraken.decode(m))
)"));
    CHECK(py.running);
    // As a listener thread would: the consumer queues while a script runs.
    BusMessage frame{.id = 0x456, .iface = 3};
    set_length(frame, 1);
    frame.data[0] = 0xAB;
    python_rx_consumer(&py, frame);
    run_to_end(app);

    const std::string out = console_text(py);
    CAPTURE(out);
    CHECK(out.find("size 0\n") != std::string::npos);
    CHECK(out.find("<kraken.Message id=0x123 dlc=2 data=01 02> 2 False True\n") != std::string::npos);
    CHECK(out.find("rx 0x456 ab 3\n") != std::string::npos);
    CHECK(out.find("ifaces []\n") != std::string::npos);
    CHECK(out.find("dbc None None\n") != std::string::npos);
    CHECK(console_text(py, true).empty());

    // A TX echo is dropped unless enable_tx_echo(); the filter gates by id.
    REQUIRE(python_run(app, py, R"(
import kraken
kraken.set_filter(0x100, mask=0x700)
print("ready")
got = [hex(f.id) for f in kraken.receive(timeout=5.0)]
print("filtered", got)
)"));
    BusMessage tx{.id = 0x101, .flags = bus_flag::tx};
    python_rx_consumer(&py, tx);
    wait_for(app, "ready\n"); // the filter is installed by then
    python_rx_consumer(&py, BusMessage{.id = 0x200});
    python_rx_consumer(&py, BusMessage{.id = 0x1FF});
    run_to_end(app);
    CHECK(console_text(py).find("filtered ['0x1ff']\n") != std::string::npos);

    // interface_id limits the filter to one interface.
    REQUIRE(python_run(app, py, R"(
import kraken
kraken.set_filter(0, mask=0, interface_id=1)
print("ready")
got = [(hex(f.id), f.interface_id) for f in kraken.receive(timeout=5.0)]
print("iface", got)
)"));
    wait_for(app, "ready\n");
    python_rx_consumer(&py, BusMessage{.id = 0x10, .iface = 0});
    python_rx_consumer(&py, BusMessage{.id = 0x11, .iface = 2});
    python_rx_consumer(&py, BusMessage{.id = 0x12, .iface = 1});
    run_to_end(app);
    CHECK(console_text(py).find("iface [('0x12', 1)]\n") != std::string::npos);

    // A full receive() queue drops frames, counts them and warns once.
    REQUIRE(python_run(app, py, R"(
import kraken, sys
print("ready")
sys.stdin.readline()
print("dropped", kraken.rx_dropped(), len(kraken.receive(timeout=0)))
)"));
    wait_for(app, "ready\n");
    for (int i = 0; i < 10005; ++i)
    {
        python_rx_consumer(&py, BusMessage{.id = 0x20});
    }
    python_input(py, "go");
    run_to_end(app);
    const std::string dropped_out = console_text(py);
    CAPTURE(dropped_out);
    CHECK(dropped_out.find("dropped 5 10000\n") != std::string::npos);
    {
        const std::lock_guard lock(log_state().mutex);
        const bool warned = std::ranges::any_of(log_state().entries, [](const LogEntry& e)
        {
            return e.text.find("receive() queue full") != std::string::npos;
        });
        CHECK(warned);
    }

    // Stop interrupts a busy script quietly (no traceback).
    REQUIRE(python_run(app, py, "import time\nwhile True:\n    time.sleep(0.005)\n"));
    std::this_thread::sleep_for(50ms);
    CHECK(py.running);
    python_stop(py);
    run_to_end(app);
    CHECK_FALSE(py.running);
    CHECK(console_text(py, true).empty());

    // Exceptions land on the console in red.
    REQUIRE(python_run(app, py, "raise ValueError('boom')\n"));
    run_to_end(app);
    CHECK(console_text(py, true).find("ValueError: boom") != std::string::npos);

    // interfaces() lists only enabled setup interfaces; send() raises instead of dropping
    // the frame on an unknown id or a closed interface (no measurement running here).
    static const DriverOps fake_ops{.name = "Fake"};
    for (const char* name : {"fake0", "fake1", "fake2"})
    {
        Iface& f = app.ifaces.emplace_back();
        f.ops = &fake_ops;
        f.info.name = name;
        f.index = static_cast<uint16_t>(app.ifaces.size() - 1);
    }
    app.setup.networks.push_back({.name = "n", .interfaces = {
        SetupInterface{.driver = "Fake", .name = "fake0"},
        SetupInterface{.driver = "Fake", .name = "fake2", .enabled = false},
        SetupInterface{.driver = "Other", .name = "fake1"}}});
    REQUIRE(python_run(app, py, R"(
import kraken
print("listed", [(i["id"], i["name"], i["state"]) for i in kraken.interfaces()])
for iid in (7, 0):
    try:
        kraken.send(kraken.Message(0x1), interface_id=iid)
        print("sent", iid)
    except Exception as e:
        print("raised", iid, type(e).__name__, e)
)"));
    run_to_end(app);
    const std::string send_out = console_text(py);
    CAPTURE(send_out);
    CHECK(send_out.find("listed [(0, 'fake0', '')]\n") != std::string::npos);
    CHECK(send_out.find("raised 7 ValueError no interface with id 7\n") != std::string::npos);
    CHECK(send_out.find("raised 0 RuntimeError interface 0 not open or send failed\n") != std::string::npos);
    CHECK(send_out.find("sent") == std::string::npos);

    // T87b a2 F6: send() without interface_id goes out on the first open (measurement) interface,
    // not on id 0 (enumerated, outside the setup).
    static std::vector<uint16_t> sent_on;
    static const DriverOps send_ops{.name = "Fake", .send = [](Iface& i, const BusMessage&) { sent_on.push_back(i.index); return true; }};
    app.ifaces[2].ops = &send_ops;
    app.ifaces[2].open = true;
    REQUIRE(python_run(app, py, "import kraken\nkraken.send(kraken.Message(0x5))\n"));
    run_to_end(app);
    CHECK(console_text(py, true).empty());
    CHECK(sent_on == std::vector<uint16_t>{2});
    app.ifaces[2].open = false;

    // T87b a2 F7: tracebacks name the file and the script's own line numbers.
    REQUIRE(python_run(app, py, "x = 1\n\ndef f():\n    1 / 0\nf()\n", "err.py"));
    run_to_end(app);
    const std::string tb = console_text(py, true);
    CAPTURE(tb);
    CHECK(tb.find("err.py(4): f") != std::string::npos);
    CHECK(tb.find("err.py(5): <module>") != std::string::npos);

    // T87b a2 F8: Stop ends a script (and a thread of it) blocked in time.sleep at once.
    REQUIRE(python_run(app, py, R"(
import threading, time
threading.Thread(target=lambda: time.sleep(30)).start()
print("sleeping")
time.sleep(30)
)"));
    wait_for(app, "sleeping");
    const auto t0 = std::chrono::steady_clock::now();
    python_stop(py);
    run_to_end(app);
    CHECK(std::chrono::steady_clock::now() - t0 < 1s);
    CHECK(console_text(py, true).empty());

    // T87c: lin_* without interface_id go to the first open LIN interface, not id 0 / a CAN one.
    static std::vector<std::pair<uint16_t, int>> lin_calls; // (iface, 0 sleep / 1 wakeup / 10+n schedule n)
    static const DriverOps lin_ops{
        .name = "Fake",
        .lin_sleep_wakeup = [](Iface& i, bool wake) { lin_calls.emplace_back(i.index, wake ? 1 : 0); },
        .lin_set_schedule = [](Iface& i, uint8_t n) { lin_calls.emplace_back(i.index, 10 + n); }};
    app.ifaces[1].ops = &lin_ops; // CAN, open, before the LIN one: must not be picked
    app.ifaces[1].open = true;
    app.ifaces[2].ops = &lin_ops;
    app.ifaces[2].info.bus_type = BusType::LIN;
    app.ifaces[2].open = true;
    REQUIRE(python_run(app, py, "import kraken\nkraken.lin_sleep()\nkraken.lin_wakeup()\nkraken.lin_set_schedule_table(2)\n"));
    run_to_end(app);
    CHECK(console_text(py, true).empty());
    CHECK(lin_calls == std::vector<std::pair<uint16_t, int>>{{2, 0}, {2, 1}, {2, 12}});
    app.ifaces[1].open = false;
    app.ifaces[2].open = false;

    // T87c: a payload length that is no CAN FD length (ISO 11898-1 Table 5) / above 8 for LIN raises.
    REQUIRE(python_run(app, py, R"(
import kraken
m = kraken.Message(0x1)
for n in (12, 64, 15, 65, -1):
    try:
        m.dlc = n
        print("dlc ok", n, m.dlc)
    except ValueError:
        print("dlc raised", n)
for n in (15, 16):
    try:
        m.set_data(bytes(n))
        print("data ok", n, m.dlc)
    except ValueError:
        print("data raised", n)
for n in (8, 9):
    try:
        print("lin ok", n, kraken.make_lin_message(0x10, n).dlc)
    except ValueError:
        print("lin raised", n)
)"));
    run_to_end(app);
    const std::string len_out = console_text(py);
    CAPTURE(len_out);
    for (const char* line : {"dlc ok 12 12\n", "dlc ok 64 64\n", "dlc raised 15\n", "dlc raised 65\n", "dlc raised -1\n",
                             "data raised 15\n", "data ok 16 16\n", "lin ok 8 8\n", "lin raised 9\n"})
    {
        CHECK(len_out.find(line) != std::string::npos);
    }

    python_shutdown(app, py);
    CHECK(py.main_thread_state == nullptr);
}
