// Setup dialog (ui/setup_dialog): page logic plus a headless draw of every page.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string_view>

#include <imgui.h>
#include <imgui_internal.h> // ClosePopupToLevel

#include "app.h"
#include "ui/setup_dialog.h"
#include "ui/theme.h"
#include "ui_test.h"

TEST_CASE("custom bitrate fields clamp Div+Seg1+Seg2 like the Qt page")
{
    CHECK(custom_bitrate_clamp(0x023407) == 0x023407);
    CHECK(custom_bitrate_clamp(0x000000) == 0x010202); // div >= 1, seg1/seg2 >= 2
    CHECK(custom_bitrate_clamp(0x0501FF) == 0x050280); // seg2 <= 128
    CHECK(custom_fd_bitrate_clamp(0x011508) == 0x011508);
    CHECK(custom_fd_bitrate_clamp(0x000000) == 0x010101);
    CHECK(custom_fd_bitrate_clamp(0xFFFFFF) == 0x202010); // 32 / 32 / 16
}

TEST_CASE("CAN page snaps to offered bitrates")
{
    IfaceInfo info{.capabilities = iface_cap::canfd,
                   .bitrates = {{.bitrate = 250000, .sample_point = 875},
                                {.bitrate = 500000, .sample_point = 750},
                                {.bitrate = 500000, .sample_point = 875},
                                {.bitrate = 500000, .bitrate_fd = 2000000, .sample_point = 875, .sample_point_fd = 750}}};
    SetupInterface intf{.bitrate = 123, .sample_point = 1, .fd_bitrate = 5000000};
    can_setup_snap(info, intf);
    CHECK(intf.bitrate == 250000);
    CHECK(intf.sample_point == 875);
    CHECK(intf.fd_bitrate == 0); // no FD timing at 250k
    CHECK_FALSE(intf.can_fd);

    intf.bitrate = 500000;
    intf.sample_point = 750;
    can_setup_snap(info, intf);
    CHECK(intf.sample_point == 750);
    CHECK(intf.fd_bitrate == 2000000);
    CHECK(intf.fd_sample_point == 750);
    CHECK(intf.can_fd);

    SetupInterface untouched{.bitrate = 42};
    can_setup_snap(IfaceInfo{}, untouched); // no list (e.g. unavailable): keep the saved value
    CHECK(untouched.bitrate == 42);
}

TEST_CASE("LIN signal raw values are written LSB first")
{
    std::vector<uint8_t> data(2, 0xFF);
    lin_signal_write_raw(data, LinSignal{.bit_offset = 3, .bit_length = 4}, 0xA); // bits 3..6 = 0,1,0,1
    CHECK(data[0] == 0xD7); // 1101 0111
    CHECK(data[1] == 0xFF);
    lin_signal_write_raw(data, LinSignal{.bit_offset = 6, .bit_length = 4}, 0x0);
    CHECK(data[0] == 0x17);
    CHECK(data[1] == 0xFC);
    lin_signal_write_raw(data, LinSignal{.bit_offset = 12, .bit_length = 8}, 0x00); // past the end: clipped
    CHECK(data[1] == 0x0C);

    LinFrame f{.length = 3, .signals = {{.bit_offset = 0, .bit_length = 16, .init_value = 0x1234},
                                        {.bit_offset = 16, .bit_length = 8, .init_value = 0x2A}}};
    CHECK(lin_frame_init_data(f) == std::vector<uint8_t>{0x34, 0x12, 0x2A});
}

namespace
{
const char* const ldf_text = R"(LIN_description_file;
LIN_protocol_version = "2.1";
LIN_language_version = "2.1";
LIN_speed = 9.6 kbps;
Nodes {
    Master: M, 5 ms, 0.1 ms ;
    Slaves: A, B ;
}
Signals {
    Speed: 16, 0x1234, M, A;
    Flag: 1, 1, A, M;
}
Frames {
    MFrame: 0x10, M, 3 {
        Speed, 0;
    }
    AFrame: 0x20, A, 1 {
        Flag, 0;
    }
}
Schedule_tables {
    First {
        MFrame delay 10 ms;
    }
    Second {
        AFrame delay 10 ms;
    }
}
)";

void frame(App& app)
{
    ImGui::NewFrame();
    draw_setup_dialog(app, app.setup_dialog);
    ImGui::EndFrame();
}
} // namespace

TEST_CASE("LDF settings are applied to a LIN interface")
{
    const auto path = std::filesystem::temp_directory_path() / "kraken_setup_dialog_test.ldf";
    std::ofstream(path) << ldf_text;
    LinDb db;
    REQUIRE(lin_db_load(db, path.string()));
    SetupInterface intf{.bus_type = BusType::LIN, .lin_protocol = LinProtocolVersion::V1_3};
    lin_setup_apply_ldf(db, intf);
    CHECK(intf.lin_ldf_path == path.string());
    CHECK(intf.lin_baudrate == 9600);
    CHECK(intf.lin_timebase_ms == 5);
    CHECK(intf.lin_jitter_us == 100);
    CHECK(intf.lin_protocol == LinProtocolVersion::V2_1);
}

TEST_CASE("Add Interfaces hides interfaces already in any network")
{
    Setup setup;
    setup.networks.push_back({.name = "A", .interfaces = {{.driver = "SocketCAN", .name = "can0"}}});
    setup.networks.push_back({.name = "B", .interfaces = {{.driver = "SLCAN", .name = "ttyACM0"}}});
    CHECK(setup_interface_used(setup, "SocketCAN", "can0"));
    CHECK(setup_interface_used(setup, "SLCAN", "ttyACM0")); // other network than the selected one
    CHECK_FALSE(setup_interface_used(setup, "SocketCAN", "can1"));
    CHECK_FALSE(setup_interface_used(setup, "SLCAN", "can0")); // same name, other driver
}

TEST_CASE("every page draws headless and edits only the copy")
{
    const auto path = std::filesystem::temp_directory_path() / "kraken_setup_dialog_draw.ldf";
    std::ofstream(path) << ldf_text;
    auto db = std::make_shared<LinDb>();
    REQUIRE(lin_db_load(*db, path.string()));

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {1280, 800};
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    theme_load_fonts(15.0f);
    {
        App app;
        app.setup.networks.push_back({.name = "CAN net", .interfaces = {{.driver = "X", .name = "can9"}}});
        app.setup.networks.push_back({.name = "LIN net",
                                      .interfaces = {{.driver = "Y", .name = "lin9", .bus_type = BusType::LIN}},
                                      .lin_dbs = {db}});
        auto& s = app.setup_dialog;
        setup_dialog_open(app, s);
        frame(app);
        frame(app);
        const ImGuiContext& g = *ImGui::GetCurrentContext(); // outside a frame: no window for IsPopupOpen(str)
        REQUIRE(g.OpenPopupStack.Size == 1);
        CHECK(std::string_view(g.OpenPopupStack[0].Window->Name) == "Measurement Setup");

        const struct { SetupSel sel; int net; int item; } pages[] = {
            {SetupSel::Network, 0, -1},   {SetupSel::Interfaces, 0, -1}, {SetupSel::Interface, 0, 0},
            {SetupSel::Databases, 1, -1}, {SetupSel::LinDb, 1, 0},       {SetupSel::Interface, 1, 0},
        };
        for (const auto& p : pages)
        {
            s.sel = p.sel;
            s.net = p.net;
            s.item = p.item;
            frame(app);
            frame(app);
        }
        // LIN page on an unavailable interface took the LDF settings and the first table.
        const SetupInterface& lin = s.work.networks[1].interfaces[0];
        CHECK(lin.lin_ldf_path == path.string());
        CHECK(lin.lin_schedule_table == "First");
        CHECK(lin.lin_baudrate == 9600);

        s.frame_defaults = {.open_request = true, .node = "M"};
        for (int i = 0; i < 3; ++i)
        {
            frame(app);
        }
        CHECK(s.work.networks[1].interfaces[0].lin_frame_defaults.at(0x10).size() == 3);

        s.add_request = true; // Add Interfaces popup on the interfaces page
        s.sel = SetupSel::Interfaces;
        s.net = 0;
        frame(app);
        frame(app);
        ImGui::ClosePopupToLevel(1, false); // back to the setup dialog
        frame(app);

        CHECK(app.setup.networks[1].interfaces[0].lin_ldf_path.empty()); // not applied before OK
    }
    ImGui::DestroyContext();
}

TEST_CASE("a network name still being edited is not written into the next network selected")
{
    const UiTest ui;
    App app;
    app.setup.networks.push_back({.name = "engine"});
    app.setup.networks.push_back({.name = "nav"});
    auto& s = app.setup_dialog;
    setup_dialog_open(app, s);
    s.sel = SetupSel::Network;
    s.net = 0;
    frame(app);
    frame(app);
    ImGuiWindow* w = ImGui::FindWindowByName("Measurement Setup");
    REQUIRE(w != nullptr);
    ImGuiIO& io = ImGui::GetIO();
    bool found = false; // the name field: the mouse shows the text cursor over it
    for (float y = w->Pos.y + 4.0f; y < w->Pos.y + w->Size.y && !found; y += 4.0f)
    {
        io.AddMousePosEvent(w->Pos.x + w->Size.x - 40.0f, y);
        frame(app);
        found = ImGui::GetMouseCursor() == ImGuiMouseCursor_TextInput;
    }
    REQUIRE(found);
    io.AddMouseButtonEvent(0, true);
    frame(app);
    io.AddMouseButtonEvent(0, false);
    frame(app);
    io.AddInputCharacter('X');
    frame(app);
    CHECK(s.work.networks[0].name == "engineX");

    ImGui::ClearActiveID(); // what a click on "nav" in the tree does, in the frame it selects it
    s.net = 1;
    frame(app);
    frame(app);
    CHECK(s.work.networks[0].name == "engineX");
    CHECK(s.work.networks[1].name == "nav");
}
