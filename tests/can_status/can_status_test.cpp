#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <format>
#include <span>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "app.h"
#include "ui/can_status.h"
#include "ui/theme.h"

namespace
{

const DriverOps fake_driver{.name = "Fake"};

struct Context
{
    Context()
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = {1600, 800};
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        theme_load_fonts(15.0f);
    }
    ~Context() { ImGui::DestroyContext(); }
};

void add_iface(App& app, const char* name, const char* details = "")
{
    Iface& i = app.ifaces.emplace_back();
    i.ops = &fake_driver;
    i.index = static_cast<uint16_t>(app.ifaces.size() - 1);
    i.info.name = name;
    i.info.details = details;
}

CanStatusRow row(IfaceState state, uint64_t rx, uint64_t rx_err, uint64_t tx, uint64_t tx_err)
{
    return {.stats = {.state = state, .rx_frames = rx, .rx_errors = rx_err, .tx_frames = tx, .tx_errors = tx_err}, .seen = true};
}

// can0 FD 500k/2M, can1 83.333k, vcan0, can2 not in the setup; made-up counters (not measuring).
void fill(App& app)
{
    add_iface(app, "can0");
    add_iface(app, "vcan0", "vcan");
    add_iface(app, "can1");
    add_iface(app, "can2");
    SetupNetwork net;
    net.interfaces.push_back({.driver = "Fake", .name = "can0", .can_fd = true});
    net.interfaces.push_back({.driver = "Fake", .name = "can1", .bitrate = 83333});
    app.setup.networks.push_back(std::move(net));
    app.can_status.rows = {row(IfaceState::Passive, 10, 1, 7, 0), row(IfaceState::Ok, 300, 0, 2, 5),
                           row(IfaceState::BusOff, 20, 9, 0, 1), row(IfaceState::Warning, 5, 3, 40, 2)};
}

void frame(App& app, const WorkspaceTab& tab)
{
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({1600, 400});
    draw_can_status(app, app.can_status, tab);
    ImGui::EndFrame();
}

ImGuiTable* table(const WorkspaceTab& tab)
{
    ImGuiWindow* w = ImGui::FindWindowByName(workspace_window_name(tab, "CAN Status").c_str());
    REQUIRE(w != nullptr);
    return ImGui::TableFindByID(w->GetID("##can_status"));
}

std::vector<uint16_t> order(const CanStatusState& s)
{
    return s.order;
}

} // namespace

TEST_CASE("rows render with a Bitrate column: setup bitrate, FD data bitrate, dash for vcan / unknown")
{
    Context ctx;
    App app;
    fill(app);
    const WorkspaceTab tab{.uid = 3};
    frame(app, tab);
    ImGuiTable* t = table(tab);
    REQUIRE(t != nullptr);
    CHECK(std::string(ImGui::TableGetColumnName(t, 3)) == "Bitrate");
    CHECK(std::string(ImGui::TableGetColumnName(t, 4)) == "State");
    const std::vector<std::string> expected = {"500k / 2M", "—", "83.333k", "—"};
    CHECK(app.can_status.bitrate == expected);
}

TEST_CASE("the driver's actual bitrate overrides the setup's; vcan and unknown stay a dash")
{
    Context ctx;
    App app;
    fill(app);
    app.can_status.rows[0].stats.bitrate = 250000; // setup says 500k / 2M, FD off in the controller
    app.can_status.rows[1].stats.bitrate = 500000; // vcan never has one; ignored anyway
    app.can_status.rows[3].stats.bitrate = 1000000; // not in the setup
    app.can_status.rows[3].stats.data_bitrate = 5000000;
    const WorkspaceTab tab{.uid = 3};
    frame(app, tab);
    const std::vector<std::string> expected = {"250k", "\u2014", "83.333k", "1M / 5M"};
    CHECK(app.can_status.bitrate == expected);
}

TEST_CASE("sort by interface, state, rx/tx frames and errors, both directions")
{
    std::deque<Iface> ifaces;
    for (const char* name : {"can0", "vcan0", "can1", "can2"})
    {
        ifaces.emplace_back().info.name = name;
    }
    CanStatusState s;
    s.rows = {row(IfaceState::Passive, 10, 1, 7, 0), row(IfaceState::Ok, 300, 0, 2, 5),
              row(IfaceState::BusOff, 20, 9, 0, 1), row(IfaceState::Warning, 5, 3, 40, 2)};
    const auto sorted = [&](CanStatusSort key, bool desc)
    {
        s.sort_key = key;
        s.sort_desc = desc;
        can_status_sort(s, ifaces);
        return s.order;
    };
    using V = std::vector<uint16_t>;
    CHECK(sorted(CanStatusSort::Interface, false) == V{0, 2, 3, 1}); // can0 can1 can2 vcan0
    CHECK(sorted(CanStatusSort::Interface, true) == V{1, 3, 2, 0});
    CHECK(sorted(CanStatusSort::State, false) == V{1, 3, 0, 2});     // ok warning passive bus-off
    CHECK(sorted(CanStatusSort::RxFrames, false) == V{3, 0, 2, 1});  // 5 10 20 300
    CHECK(sorted(CanStatusSort::RxFrames, true) == V{1, 2, 0, 3});
    CHECK(sorted(CanStatusSort::RxErrors, false) == V{1, 0, 3, 2});  // 0 1 3 9
    CHECK(sorted(CanStatusSort::TxFrames, false) == V{2, 1, 0, 3});  // 0 2 7 40
    CHECK(sorted(CanStatusSort::TxErrors, true) == V{1, 3, 2, 0});   // 5 2 1 0
    CHECK_FALSE(s.sort_dirty);
    // Not yet polled: zero counters, ties keep the interface order.
    s.rows.resize(2);
    CHECK(sorted(CanStatusSort::RxFrames, false) == V{2, 3, 0, 1}); // 0 0 10 300
}

TEST_CASE("clicking the Rx Frames header sorts ascending, again descending")
{
    Context ctx;
    App app;
    fill(app);
    const WorkspaceTab tab{.uid = 4};
    frame(app, tab);
    CHECK(order(app.can_status) == std::vector<uint16_t>{0, 2, 3, 1}); // default: Interface ascending
    ImGuiTable* t = table(tab);
    REQUIRE(t != nullptr);
    const ImGuiTableColumn& col = t->Columns[5];
    REQUIRE(std::string(ImGui::TableGetColumnName(t, 5)) == "Rx Frames");
    const ImVec2 at{(col.MinX + col.MaxX) * 0.5f, t->OuterRect.Min.y + ImGui::GetFrameHeight() * 0.5f};
    ImGuiIO& io = ImGui::GetIO();
    const auto click = [&]
    {
        io.AddMousePosEvent(at.x, at.y);
        frame(app, tab);
        io.AddMouseButtonEvent(0, true);
        frame(app, tab);
        io.AddMouseButtonEvent(0, false);
        frame(app, tab);
        frame(app, tab);
    };
    click();
    CHECK(app.can_status.sort_key == CanStatusSort::RxFrames);
    CHECK(order(app.can_status) == std::vector<uint16_t>{3, 0, 2, 1});
    click();
    CHECK(app.can_status.sort_desc);
    CHECK(order(app.can_status) == std::vector<uint16_t>{1, 2, 0, 3});
}

TEST_CASE("j / k / G / gg move the selection along the display order")
{
    Context ctx;
    App app;
    fill(app);
    const WorkspaceTab tab{.uid = 5};
    frame(app, tab); // the new window takes focus; order can0 can1 can2 vcan0 = 0 2 3 1
    CHECK(app.can_status.selected == -1);
    const auto type = [&](ImWchar c)
    {
        ImGui::GetIO().AddInputCharacter(c);
        frame(app, tab);
    };
    type('j');
    CHECK(app.can_status.selected == 0);
    type('j');
    CHECK(app.can_status.selected == 2);
    type('j');
    CHECK(app.can_status.selected == 3);
    type('k');
    CHECK(app.can_status.selected == 2);
    type('G');
    CHECK(app.can_status.selected == 1);
    type('g');
    type('g');
    CHECK(app.can_status.selected == 0);
}

TEST_CASE("error frames are counted per interface from the trace, only the rows appended since the last call")
{
    Context ctx;
    App app;
    fill(app);
    const WorkspaceTab tab{.uid = 3};
    const auto add = [&](BusMessage m) { trace_append(app.trace, std::span(&m, 1)); };
    add(BusMessage{.id = 1, .errors = bus_error::generic, .iface = 0});
    add(BusMessage{.id = 2, .iface = 0});
    add(BusMessage{.id = 3, .errors = bus_error::generic, .iface = 2});
    frame(app, tab);
    CHECK(app.can_status.rows[0].error_frames == 1);
    CHECK(app.can_status.rows[1].error_frames == 0);
    CHECK(app.can_status.rows[2].error_frames == 1);
    ImGuiTable* t = table(tab);
    REQUIRE(t != nullptr);
    CHECK(std::string(ImGui::TableGetColumnName(t, 8)) == "Error Frames");
    frame(app, tab); // nothing new: counts stay
    CHECK(app.can_status.rows[0].error_frames == 1);
    add(BusMessage{.id = 4, .errors = bus_error::generic, .iface = 0});
    frame(app, tab);
    CHECK(app.can_status.rows[0].error_frames == 2);
}

TEST_CASE("an interface whose listener failed shows stopped, not its last state (T87b a2 F9)")
{
    Context ctx;
    App app;
    fill(app);
    app.ifaces[1].open = true;
    app.ifaces[1].failed = true;
    app.measuring = true;
    app.can_status.was_measuring = true; // mid-measurement: no re-base of the rows
    const WorkspaceTab tab{.uid = 3};
    frame(app, tab);
    CHECK(app.can_status.rows[1].stats.state == IfaceState::Stopped);
    CHECK(app.can_status.rows[1].stats.rx_frames == 300); // last counters kept
    CHECK(app.can_status.rows[0].stats.state == IfaceState::Passive); // closed, not failed: unchanged
    app.measuring = false;
    app.ifaces[1].open = false;
}

TEST_CASE("Delete on a vcan link asks first; Cancel deletes nothing (T87c)")
{
    Context ctx;
    App app;
    static const DriverOps socketcan{.name = "SocketCAN"};
    Iface& vcan = app.ifaces.emplace_back();
    vcan.ops = &socketcan;
    vcan.info.name = "vcan9";
    vcan.info.details = "vcan";
    ImGuiID delete_id = 0;
    ImGuiID popup_id = 0;
    const auto draw = [&]
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::Begin("links");
        draw_link_buttons(app, vcan);
        ImGui::PushID(vcan.index); // the ids draw_link_buttons uses
        delete_id = ImGui::GetID("Delete");
        popup_id = ImGui::GetID("confirm_delete");
        ImGui::PopID();
        ImGui::End();
        ImGui::EndFrame();
    };
    draw();
    ImGui::ActivateItemByID(delete_id);
    for (int i = 0; i < 3; ++i)
    {
        draw();
    }
    CHECK(ImGui::IsPopupOpen(popup_id, ImGuiPopupFlags_None));
    CHECK_FALSE(app.can_status.link_busy); // no ip command started yet

    ImGuiWindow* popup = ImGui::FindWindowByName(std::format("##Popup_{:08x}", popup_id).c_str());
    REQUIRE(popup != nullptr);
    ImGui::ActivateItemByID(ImHashStr("Cancel", 0, popup->ID));
    for (int i = 0; i < 3; ++i)
    {
        draw();
    }
    CHECK_FALSE(ImGui::IsPopupOpen(popup_id, ImGuiPopupFlags_None));
    CHECK_FALSE(app.can_status.link_busy);
}
