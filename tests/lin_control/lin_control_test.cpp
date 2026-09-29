// ui/lin_control: the guarded LIN driver calls against a fake DriverOps (only while open,
// only when supported), the diag request hex codec and the workspace XML round trip.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <thread>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <pugixml.hpp>

#include "app.h"

#include "drivers/driver.h"
#include "ui/lin_control.h"

namespace
{

struct Calls
{
    int sleep = 0;
    int wakeup = 0;
    int schedule = -1;
    std::vector<uint8_t> diag;
    uint8_t nad = 0;
};

Calls calls;

int fake_read(Iface&, BusMessage*, int, int timeout_ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 5)));
    return 0;
}

const DriverOps fake_lin = {
    .name = "FakeLin",
    .enumerate = nullptr,
    .open = [](Iface&, const IfaceConfig&) { return true; },
    .close = [](Iface&) {},
    .send = [](Iface&, const BusMessage&) { return true; },
    .read = fake_read,
    .stats = nullptr,
    .lin_sleep_wakeup = [](Iface&, bool wakeup) { ++(wakeup ? calls.wakeup : calls.sleep); },
    .lin_set_schedule = [](Iface&, uint8_t table) { calls.schedule = table; },
    .lin_diag_request =
        [](Iface&, uint8_t nad, std::span<const uint8_t> data)
    {
        calls.nad = nad;
        calls.diag.assign(data.begin(), data.end());
    },
};

const DriverOps fake_can = {
    .name = "FakeCan",
    .enumerate = nullptr,
    .open = [](Iface&, const IfaceConfig&) { return true; },
    .close = [](Iface&) {},
    .send = [](Iface&, const BusMessage&) { return true; },
    .read = fake_read,
    .stats = nullptr,
    .lin_sleep_wakeup = nullptr,
    .lin_set_schedule = nullptr,
    .lin_diag_request = nullptr,
};

} // namespace

TEST_CASE("LIN calls reach the driver only while the interface is open and supports them")
{
    std::deque<Iface> ifaces;
    Iface& lin = ifaces.emplace_back();
    lin.ops = &fake_lin;
    lin.info.name = "lin0";
    lin.info.bus_type = BusType::LIN;
    Iface& can = ifaces.emplace_back();
    can.ops = &fake_can;
    can.info.name = "can0";
    const uint8_t req[] = {0x22, 0xF1, 0x90};

    // Closed: nothing goes out.
    CHECK_FALSE(lin_send_sleep_wakeup(lin, true));
    CHECK_FALSE(lin_send_set_schedule(lin, 1));
    CHECK_FALSE(lin_send_diag_request(lin, 0x7F, req));
    CHECK(calls.wakeup == 0);
    CHECK(calls.schedule == -1);

    Setup setup;
    setup.networks.push_back({.interfaces = {{.driver = "FakeLin", .name = "lin0", .bus_type = BusType::LIN},
                                             {.driver = "FakeCan", .name = "can0"}}});
    REQUIRE(ifaces_start(ifaces, setup, {}, nullptr) == 2);
    CHECK(lin_send_sleep_wakeup(lin, true));
    CHECK(lin_send_sleep_wakeup(lin, false));
    CHECK(lin_send_set_schedule(lin, 2));
    CHECK(lin_send_diag_request(lin, 0x7F, req));
    CHECK_FALSE(lin_send_diag_request(lin, 0x7F, {})); // nothing to send
    CHECK(calls.wakeup == 1);
    CHECK(calls.sleep == 1);
    CHECK(calls.schedule == 2);
    CHECK(calls.nad == 0x7F);
    CHECK(calls.diag == std::vector<uint8_t>{0x22, 0xF1, 0x90});
    // A driver without LIN support: false, no crash.
    CHECK_FALSE(lin_send_sleep_wakeup(can, true));
    CHECK_FALSE(lin_send_set_schedule(can, 0));
    CHECK_FALSE(lin_send_diag_request(can, 1, req));
    ifaces_stop(ifaces);
    CHECK_FALSE(lin_send_sleep_wakeup(lin, true));
    CHECK(calls.wakeup == 1);
}

TEST_CASE("diag request hex codec")
{
    CHECK(parse_hex_bytes("22 F1 90") == std::vector<uint8_t>{0x22, 0xF1, 0x90});
    CHECK(parse_hex_bytes("22f190") == std::vector<uint8_t>{0x22, 0xF1, 0x90});
    CHECK(parse_hex_bytes("").value().empty());
    CHECK_FALSE(parse_hex_bytes("22 F").has_value());  // odd digit count
    CHECK_FALSE(parse_hex_bytes("22 G1").has_value()); // not hex
    const uint8_t bytes[] = {0x0A, 0xFF};
    CHECK(format_hex_bytes(bytes) == "0A FF");
    CHECK(format_hex_bytes({}).empty());
}

TEST_CASE("workspace XML round trip resolves the interface by driver + name")
{
    std::deque<Iface> ifaces;
    Iface& lin = ifaces.emplace_back();
    lin.ops = &fake_lin;
    lin.info.name = "lin0";
    LinControl lc;
    lc.requests.push_back({.name = "Read Product ID", .iface = 0, .nad = 0x7F, .data = {0x22, 0xF1, 0x90}});
    lc.requests.push_back({.name = "Unresolved", .iface = UINT16_MAX, .nad = 1, .data = {0x3E}});

    pugi::xml_document doc;
    pugi::xml_node root = doc.append_child("lincontrolwindow");
    lin_control_save_xml(lc, ifaces, root);
    CHECK(std::string(root.child("DiagRequest").attribute("data").as_string()) == "22 F1 90");
    CHECK(std::string(root.child("DiagRequest").attribute("interface").as_string()) == "lin0");

    LinControl loaded;
    lin_control_load_xml(loaded, ifaces, root);
    REQUIRE(loaded.requests.size() == 2);
    CHECK(loaded.requests[0].name == "Read Product ID");
    CHECK(loaded.requests[0].iface == 0);
    CHECK(loaded.requests[0].nad == 0x7F);
    CHECK(loaded.requests[0].data == std::vector<uint8_t>{0x22, 0xF1, 0x90});
    CHECK(loaded.requests[1].iface == UINT16_MAX);
    CHECK(loaded.requests[1].data == std::vector<uint8_t>{0x3E});
}

TEST_CASE("the T31 windows render: LIN Control + diag modal, GPIO Control, Conditional Logging")
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {1280, 800};
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    theme_load_fonts(15.0f);
    {
        App app;
        const WorkspaceTab& tab = workspace_add_tab(app.workspace);
        LinControl& lc = app.lin_controls[tab.uid];
        lc.open = true;
        lc.requests.push_back({.name = "Read Product ID", .nad = 0x7F, .data = {0x22, 0xF1, 0x90}});
        diag_dialog_open(lc.dialog, &lc.requests[0]);
        conditional_logging_open(app.conditional_logging);
        for (int i = 0; i < 3; ++i)
        {
            ImGui::NewFrame();
            draw_lin_control(app, tab, lc);
            conditional_logging_frame(app, app.conditional_logging);
            draw_conditional_logging(app, app.conditional_logging);
            ImGui::EndFrame();
        }
        CHECK(ImGui::FindWindowByName(workspace_window_name(tab, "LIN Control").c_str()) != nullptr);
        CHECK(ImGui::FindWindowByName("Conditional Logging Configuration") != nullptr);
        CHECK(lc.dialog.open);
    }
    ImGui::DestroyContext();
}
