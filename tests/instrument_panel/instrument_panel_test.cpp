// ui/instrument_panel: last value per bound signal from new trace frames, input-widget encoding
// into frame bytes, sending through iface_send, the workspace XML round trip and headless drawing.
// Expected values are computed by hand from the DBC factor / offset / bit layout below.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>

#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <pugixml.hpp>

#include "app.h"

#include "core/trace.h"
#include "db/dbc/dbc_parser.h"
#include "db/model/can_db.h"
#include "drivers/driver.h"
#include "ui/instrument_panel.h"

namespace
{

// Pressure: Motorola, DBC start bit 7 (MSB of byte 0), bytes 0-1 big-endian, 0.1 bar.
// Speed: Intel bytes 2-3, 0.25 rpm. Temp: byte 4, offset -40. Horn: bit 0 of byte 5.
constexpr const char* dbc = R"(VERSION ""

BU_: ECU

BO_ 291 Engine: 8 ECU
 SG_ Pressure : 7|16@0+ (0.1,0) [0|6553.5] "bar" ECU
 SG_ Speed : 16|16@1+ (0.25,0) [0|16383.75] "rpm" ECU
 SG_ Temp : 32|8@1+ (1,-40) [-40|215] "degC" ECU
 SG_ Horn : 40|1@1+ (1,0) [0|1] "" ECU
)";

BusMessage sent;
int sends = 0;

int fake_read(Iface&, BusMessage*, int, int timeout_ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 5)));
    return 0;
}

const DriverOps fake_can = {
    .name = "FakeCan",
    .enumerate = nullptr,
    .open = [](Iface&, const IfaceConfig&) { return true; },
    .close = [](Iface&) {},
    .send = [](Iface&, const BusMessage& m)
    {
        sent = m;
        ++sends;
        return true;
    },
    .read = fake_read,
    .stats = nullptr,
    .lin_sleep_wakeup = nullptr,
    .lin_set_schedule = nullptr,
    .lin_diag_request = nullptr,
};

void add_db(Setup& setup)
{
    auto db = std::make_shared<CanDb>();
    REQUIRE(dbc_parse(dbc, *db));
    setup.networks.push_back({.name = "Net", .can_dbs = {db}});
    setup_rebuild_cache(setup);
}

Instrument bound(InstrumentKind kind, const char* signal)
{
    return {.kind = kind, .network = "Net", .raw_id = 291, .signal = signal};
}

BusMessage engine_frame(std::array<uint8_t, 8> bytes, int64_t ts)
{
    BusMessage m{.id = 0x123, .ts_ns = ts};
    set_length(m, 8);
    std::copy(bytes.begin(), bytes.end(), m.data.begin());
    return m;
}

} // namespace

TEST_CASE("last value per bound signal follows the frames appended to the trace")
{
    Setup setup;
    add_db(setup);
    InstrumentPanel p;
    p.items = {bound(InstrumentKind::Gauge, "Speed"), bound(InstrumentKind::Numeric, "Pressure"),
               bound(InstrumentKind::Led, "Temp"), bound(InstrumentKind::Trend, "Speed"),
               bound(InstrumentKind::Bar, "Missing")};
    Trace trace;
    instrument_panel_ingest(p, setup, trace);
    REQUIRE(p.items[0].sig != nullptr);
    CHECK(p.items[4].sig == nullptr); // stays, shown unbound
    CHECK_FALSE(p.items[0].has_value);

    // Pressure 0x01A9 = 425 -> 42.5 bar; Speed 0x36B0 = 14000 -> 3500 rpm; Temp 0x82 = 130 -> 90 degC.
    BusMessage other{.id = 0x124};
    set_length(other, 8);
    const std::array<BusMessage, 2> batch = {engine_frame({0x01, 0xA9, 0xB0, 0x36, 0x82, 0, 0, 0}, 1000), other};
    trace_append(trace, batch);
    instrument_panel_ingest(p, setup, trace);
    CHECK(p.items[0].value == doctest::Approx(3500.0));
    CHECK(p.items[1].value == doctest::Approx(42.5));
    CHECK(p.items[2].value == doctest::Approx(90.0));
    CHECK(p.items[3].trend.size() == 1);
    CHECK_FALSE(p.items[4].has_value);

    // Only the new frame is decoded: Speed 0x0FA0 = 4000 -> 1000 rpm; Temp 0x00 -> -40 degC.
    const std::array<BusMessage, 1> next = {engine_frame({0x00, 0x7B, 0xA0, 0x0F, 0x00, 0, 0, 0}, 2000)};
    trace_append(trace, next);
    instrument_panel_ingest(p, setup, trace);
    CHECK(p.items[0].value == doctest::Approx(1000.0));
    CHECK(p.items[1].value == doctest::Approx(12.3));
    CHECK(p.items[2].value == doctest::Approx(-40.0));
    CHECK(p.items[3].trend.size() == 2);
    instrument_panel_ingest(p, setup, trace); // nothing new
    CHECK(p.items[3].trend.size() == 2);

    trace_clear(trace);
    instrument_panel_ingest(p, setup, trace);
    CHECK_FALSE(p.items[0].has_value);
}

TEST_CASE("input widgets encode into the message's bytes")
{
    Setup setup;
    add_db(setup);
    InstrumentPanel p;
    p.items = {bound(InstrumentKind::Button, "Horn"), bound(InstrumentKind::Slider, "Speed"),
               bound(InstrumentKind::Slider, "Pressure")};
    Trace trace;
    instrument_panel_ingest(p, setup, trace);

    // Nothing seen yet: zeroed frame of the DBC length, id 0x123, Horn = bit 0 of byte 5.
    BusMessage m = instrument_encode(p.items[0], 1.0);
    CHECK(m.id == 0x123);
    CHECK(m.len == 8);
    CHECK_FALSE(has_flag(m, bus_flag::extended));
    CHECK(m.data[5] == 0x01);
    CHECK(std::all_of(m.data.begin(), m.data.begin() + 5, [](uint8_t b) { return b == 0; }));

    // After a frame: the other signals keep their bytes. Speed 1000 rpm -> 4000 = 0x0FA0, Intel.
    const std::array<BusMessage, 1> batch = {engine_frame({0x01, 0xA9, 0xB0, 0x36, 0x82, 0, 0, 0}, 1)};
    trace_append(trace, batch);
    instrument_panel_ingest(p, setup, trace);
    m = instrument_encode(p.items[1], 1000.0);
    CHECK(m.data[0] == 0x01);
    CHECK(m.data[1] == 0xA9);
    CHECK(m.data[2] == 0xA0);
    CHECK(m.data[3] == 0x0F);
    CHECK(m.data[4] == 0x82);
    // Pressure 12.3 bar -> 123 = 0x007B, Motorola: byte 0 = 0x00, byte 1 = 0x7B.
    m = instrument_encode(p.items[2], 12.3);
    CHECK(m.data[0] == 0x00);
    CHECK(m.data[1] == 0x7B);
    CHECK(m.data[2] == 0xB0);
}

TEST_CASE("instrument_send goes out through iface_send on the chosen interface")
{
    App app;
    Iface& can = app.ifaces.emplace_back();
    can.ops = &fake_can;
    can.info.name = "can0";
    add_db(app.setup);
    app.setup.networks[0].interfaces.push_back({.driver = "FakeCan", .name = "can0"});
    InstrumentPanel p;
    p.items = {bound(InstrumentKind::Checkbox, "Horn")};
    instrument_panel_ingest(p, app.setup, app.trace);
    CHECK_FALSE(instrument_send(app, p.items[0], 1.0)); // no interface chosen
    p.items[0].iface = 0;
    CHECK_FALSE(instrument_send(app, p.items[0], 1.0)); // interface closed
    REQUIRE(ifaces_start(app.ifaces, app.setup, {}, nullptr) == 1);
    CHECK(instrument_send(app, p.items[0], 1.0));
    CHECK(sends == 1);
    CHECK(sent.id == 0x123);
    CHECK(sent.data[5] == 0x01);
    ifaces_stop(app.ifaces);
}

TEST_CASE("workspace XML round trip keeps layout and bindings")
{
    std::deque<Iface> ifaces;
    Iface& can = ifaces.emplace_back();
    can.ops = &fake_can;
    can.info.name = "can0";
    InstrumentPanel p;
    p.columns = 6;
    p.edit = false;
    Instrument g = bound(InstrumentKind::Gauge, "Speed");
    g.label = "RPM";
    g.col = 2;
    g.row = 1;
    g.col_span = 2;
    g.row_span = 3;
    g.min = 0;
    g.max = 8000;
    g.red_from = 6500;
    Instrument l = bound(InstrumentKind::Led, "Temp");
    l.led = LedCondition::Above;
    l.threshold = 100;
    Instrument b = bound(InstrumentKind::Button, "Horn");
    b.iface = 0;
    b.toggle = true;
    b.on_value = 1;
    b.off_value = 0;
    Instrument t = bound(InstrumentKind::Trend, "Pressure");
    t.trend_len = 250;
    Instrument v = bound(InstrumentKind::Bar, "Speed");
    v.vertical = true;
    Instrument k = bound(InstrumentKind::Knob, "Pressure");
    k.iface = 0;
    k.min = 1.5;
    k.max = 12.5;
    k.col = 3;
    p.items = {g, l, b, t, v, k};

    pugi::xml_document doc;
    pugi::xml_node el = doc.append_child("instrumentpanel");
    instrument_panel_save_xml(p, ifaces, el);
    InstrumentPanel q;
    instrument_panel_load_xml(q, ifaces, el);
    CHECK(q.columns == 6);
    CHECK_FALSE(q.edit);
    REQUIRE(q.items.size() == 6);
    CHECK(q.items[0].kind == InstrumentKind::Gauge);
    CHECK(q.items[0].label == "RPM");
    CHECK(q.items[0].col == 2);
    CHECK(q.items[0].row == 1);
    CHECK(q.items[0].col_span == 2);
    CHECK(q.items[0].row_span == 3);
    CHECK(q.items[0].network == "Net");
    CHECK(q.items[0].raw_id == 291);
    CHECK(q.items[0].signal == "Speed");
    CHECK(q.items[0].max == 8000);
    CHECK(q.items[0].red_from == 6500);
    CHECK(q.items[0].iface == UINT16_MAX);
    CHECK(q.items[1].led == LedCondition::Above);
    CHECK(q.items[1].threshold == 100);
    CHECK(q.items[2].kind == InstrumentKind::Button);
    CHECK(q.items[2].iface == 0);
    CHECK(q.items[2].toggle);
    CHECK(q.items[3].trend_len == 250);
    CHECK(q.items[4].vertical);
    CHECK(q.items[5].kind == InstrumentKind::Knob);
    CHECK(q.items[5].signal == "Pressure");
    CHECK(q.items[5].iface == 0);
    CHECK(q.items[5].min == 1.5);
    CHECK(q.items[5].max == 12.5);
    CHECK(q.items[5].col == 3);
}

TEST_CASE("knob drag turns into a snapped physical value and the message's bytes")
{
    Setup setup;
    add_db(setup);
    InstrumentPanel p;
    p.items = {bound(InstrumentKind::Knob, "Speed"), bound(InstrumentKind::Knob, "Pressure")};
    Trace trace;
    instrument_panel_ingest(p, setup, trace);
    Instrument& speed = p.items[0];
    speed.min = 0;
    speed.max = 8000;
    // 50 px up of 200 px per full range = +2000 rpm: 1000 -> 3000 rpm -> 12000 = 0x2EE0, Intel bytes 2-3.
    const double v = instrument_knob_drag(speed, 1000.0, -50.0f, 200.0f);
    CHECK(v == doctest::Approx(3000.0));
    BusMessage m = instrument_encode(speed, v);
    CHECK(m.data[2] == 0xE0);
    CHECK(m.data[3] == 0x2E);
    // Clamped to max: 8000 rpm -> 32000 = 0x7D00.
    m = instrument_encode(speed, instrument_knob_drag(speed, 1000.0, -1000.0f, 200.0f));
    CHECK(m.data[2] == 0x00);
    CHECK(m.data[3] == 0x7D);
    // Downwards past min clamps to 0.
    CHECK(instrument_knob_drag(speed, 1000.0, 400.0f, 200.0f) == 0.0);

    // Pressure resolution 0.1 bar: 12.0 + 3.4 px of 1000 px per 0..100 = 12.34 -> 12.3 bar -> 123 = 0x007B, Motorola.
    Instrument& pressure = p.items[1];
    pressure.min = 0;
    pressure.max = 100;
    const double pv = instrument_knob_drag(pressure, 12.0, -3.4f, 1000.0f);
    CHECK(pv == doctest::Approx(12.3));
    m = instrument_encode(pressure, pv);
    CHECK(m.data[0] == 0x00);
    CHECK(m.data[1] == 0x7B);
}

TEST_CASE("dragging a widget onto an occupied cell swaps them, the corner changes its span")
{
    InstrumentPanel p;
    p.items = {{.kind = InstrumentKind::Gauge, .col = 0, .row = 0}, {.kind = InstrumentKind::Led, .col = 2, .row = 1, .col_span = 2}};
    instrument_panel_move(p, 0, 3, 1); // inside the LED's 2-wide span
    CHECK(p.items[0].col == 3);
    CHECK(p.items[0].row == 1);
    CHECK(p.items[1].col == 0);
    CHECK(p.items[1].row == 0);
    instrument_panel_move(p, 1, 1, 4); // free cell: nobody else moves
    CHECK(p.items[1].col == 1);
    CHECK(p.items[1].row == 4);
    CHECK(p.items[0].col == 3);
    instrument_panel_move(p, 1, -2, -1);
    CHECK(p.items[1].col == 0);
    CHECK(p.items[1].row == 0);

    instrument_resize(p.items[0], 3, 2);
    CHECK(p.items[0].col_span == 3);
    CHECK(p.items[0].row_span == 2);
    instrument_resize(p.items[0], 0, 99);
    CHECK(p.items[0].col_span == 1);
    CHECK(p.items[0].row_span == 16);
}

TEST_CASE("gauge texts do not collide down to the smallest cell")
{
    const auto overlap = [](const InstrumentRect& a, const InstrumentRect& b)
    { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; };
    const auto inside = [](const InstrumentRect& a, float w, float h)
    { return a.x0 >= 0.0f && a.y0 >= 0.0f && a.x1 <= w && a.y1 <= h; };
    // Nearest / farthest point of a rect to the dial centre.
    const auto near_dist = [](const InstrumentRect& a, float cx, float cy)
    { return std::hypot(std::clamp(cx, a.x0, a.x1) - cx, std::clamp(cy, a.y0, a.y1) - cy); };
    const auto far_dist = [](const InstrumentRect& a, float cx, float cy)
    { return std::hypot(std::max(cx - a.x0, a.x1 - cx), std::max(cy - a.y0, a.y1 - cy)); };

    constexpr float fs = 15.0f;
    // "3700 rpm", "0", "4000" at ~0.55 fs per character. Cell sizes: the panel's minimum (3 fs wide),
    // 1x1 of 4 columns in the default window, and bigger.
    for (const auto [w, h] : {std::pair{26.0f, 66.0f}, {60.0f, 66.0f}, {130.0f, 66.0f}, {280.0f, 170.0f}, {600.0f, 400.0f}})
    {
        CAPTURE(w);
        CAPTURE(h);
        const GaugeLayout g = instrument_gauge_layout(w, h, fs, 8 * 8.25f, 8.25f, 4 * 8.25f);
        CHECK(g.cy - g.r >= 0.0f); // dial below the title line
        CHECK(g.cy + g.r <= h);
        CHECK(g.cx + g.r <= w);
        CHECK(g.value_scale > 0.0f);
        CHECK(far_dist(g.value, g.cx, g.cy) <= g.r); // value inside the dial
        CHECK(g.value.y0 > g.cy);                    // below the centre
        if (g.scale_labels)
        {
            CHECK(inside(g.lo, w, h));
            CHECK(inside(g.hi, w, h));
            CHECK(near_dist(g.lo, g.cx, g.cy) >= g.r); // outside the arc
            CHECK(near_dist(g.hi, g.cx, g.cy) >= g.r);
            CHECK_FALSE(overlap(g.lo, g.hi));
            CHECK_FALSE(overlap(g.lo, g.value));
            CHECK_FALSE(overlap(g.hi, g.value));
        }
    }
    CHECK(instrument_gauge_layout(600.0f, 400.0f, fs, 66.0f, 8.25f, 33.0f).scale_labels);
    CHECK(instrument_gauge_layout(600.0f, 400.0f, fs, 66.0f, 8.25f, 33.0f).value_scale == 1.0f);
}

TEST_CASE("the panel draws every widget kind headless")
{
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {1280, 800};
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    theme_load_fonts(15.0f);
    {
        App app;
        add_db(app.setup);
        const WorkspaceTab& tab = workspace_add_tab(app.workspace);
        InstrumentPanel& p = app.instrument_panels[tab.uid];
        p.open = true;
        for (int k = 0; k < static_cast<int>(InstrumentKind::Count); ++k)
        {
            Instrument inst = bound(static_cast<InstrumentKind>(k), "Speed");
            inst.col = k % 4;
            inst.row = k / 4;
            p.items.push_back(inst);
        }
        p.items.push_back({.kind = InstrumentKind::Gauge, .row = 3}); // unbound
        const std::array<BusMessage, 1> batch = {engine_frame({0x01, 0xA9, 0xB0, 0x36, 0x82, 0, 0, 0}, 1)};
        trace_append(app.trace, batch);
        for (const bool edit : {true, false})
        {
            p.edit = edit;
            for (int i = 0; i < 3; ++i)
            {
                ImGui::NewFrame();
                instrument_panel_ingest(p, app.setup, app.trace);
                draw_instrument_panel(app, tab, p);
                ImGui::EndFrame();
            }
        }
        CHECK(ImGui::FindWindowByName(workspace_window_name(tab, "Instrument Panel").c_str()) != nullptr);
        CHECK(p.items[0].value == doctest::Approx(3500.0));
    }
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
}
