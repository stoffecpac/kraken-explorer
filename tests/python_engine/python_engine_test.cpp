#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

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
import cangaroo
print("size", cangaroo.trace_size())
m = cangaroo.Message(0x123)
m.set_data(b"\x01\x02")
print(repr(m), m.dlc, m.extended, m.is_rx)
for f in cangaroo.receive(timeout=5.0):
    print("rx", hex(f.id), f.get_data().hex(), f.interface_id)
print("ifaces", cangaroo.interfaces())
print("dbc", cangaroo.find_message("Nothing"), cangaroo.decode(m))
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
    CHECK(out.find("<cangaroo.Message id=0x123 dlc=2 data=01 02> 2 False True\n") != std::string::npos);
    CHECK(out.find("rx 0x456 ab 3\n") != std::string::npos);
    CHECK(out.find("ifaces []\n") != std::string::npos);
    CHECK(out.find("dbc None None\n") != std::string::npos);
    CHECK(console_text(py, true).empty());

    // A TX echo is dropped unless enable_tx_echo(); the filter gates by id.
    REQUIRE(python_run(app, py, R"(
import cangaroo
cangaroo.set_filter(0x100, mask=0x700)
print("ready")
got = [hex(f.id) for f in cangaroo.receive(timeout=5.0)]
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
import cangaroo
cangaroo.set_filter(0, mask=0, interface_id=1)
print("ready")
got = [(hex(f.id), f.interface_id) for f in cangaroo.receive(timeout=5.0)]
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
import cangaroo, sys
print("ready")
sys.stdin.readline()
print("dropped", cangaroo.rx_dropped(), len(cangaroo.receive(timeout=0)))
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
import cangaroo
print("listed", [(i["id"], i["name"], i["state"]) for i in cangaroo.interfaces()])
for iid in (7, 0):
    try:
        cangaroo.send(cangaroo.Message(0x1), interface_id=iid)
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

    python_shutdown(app, py);
    CHECK(py.main_thread_state == nullptr);
}
