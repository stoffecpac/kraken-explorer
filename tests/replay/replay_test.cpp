// ui/replay: trace file parsing (vectors written by hand from the candump, Vector ASC,
// pcap and pcapng formats, not from Kraken Explorer's writers), the filter/plan, and playback
// on vcan0 with the original timing (skipped when vcan0 is not up).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "app.h"
#include "ui/replay.h"
#include "ui/workspace_tabs.h"
#include "ui_test.h"

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

extern const DriverOps socketcan_driver;

namespace
{

void le32(std::string& s, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
    {
        s += static_cast<char>(v >> (8 * i));
    }
}

void le16(std::string& s, uint16_t v)
{
    s += static_cast<char>(v);
    s += static_cast<char>(v >> 8);
}

// struct can_frame with can_id in network byte order (LINKTYPE_CAN_SOCKETCAN).
std::string socketcan_frame(uint32_t can_id, std::vector<uint8_t> data)
{
    std::string s;
    for (int i = 3; i >= 0; --i)
    {
        s += static_cast<char>(can_id >> (8 * i));
    }
    s += static_cast<char>(data.size());
    s.append(3, '\0');
    data.resize(8);
    s.append(data.begin(), data.end());
    return s;
}

} // namespace

TEST_CASE("candump -L lines")
{
    const ReplayFile f = replay_parse("(1700000000.000000) vcan0 123#DEADBEEF\n"
                                      "(1700000000.010000) vcan0 00000456#00\n"
                                      "(1700000000.020000) vcan1 321##1AABB\n"
                                      "(1700000000.030000) vcan0 7FF#R\n"
                                      "(1700000000.040000) vcan0 20000004#0000000000000000\n"
                                      "garbage\n",
                                      TraceFileFormat::CanDump);
    REQUIRE(f.frames.size() == 5);
    REQUIRE(f.channels == std::vector<std::string>{"vcan0", "vcan1"});
    CHECK(f.frames[0].id == 0x123);
    CHECK(f.frames[0].ts_ns == 1700000000000000000);
    CHECK(f.frames[0].len == 4);
    CHECK(f.frames[0].data[3] == 0xEF);
    CHECK(!has_flag(f.frames[0], bus_flag::extended));
    CHECK(has_flag(f.frames[1], bus_flag::extended)); // 8 hex digits
    CHECK(f.frames[1].ts_ns - f.frames[0].ts_ns == 10000000);
    CHECK(f.frames[2].iface == 1);
    CHECK(has_flag(f.frames[2], bus_flag::fd));
    CHECK(has_flag(f.frames[2], bus_flag::brs));
    CHECK(f.frames[2].len == 2);
    CHECK(f.frames[2].data[1] == 0xBB);
    CHECK(has_flag(f.frames[3], bus_flag::rtr));
    CHECK(is_error_frame(f.frames[4]));
}

TEST_CASE("Vector ASC events")
{
    const ReplayFile f = replay_parse("date Mon Sep 25 10:00:00.000 am 2026\n"
                                      "base hex  timestamps absolute\n"
                                      "Begin TriggerBlock Mon Sep 25 10:00:00.000 am 2026\n"
                                      "   0.000000 1  123             Rx   d 2 11 22  Length = 0 BitCount = 0 ID = 291\n"
                                      "   0.500000 2  1ABCDEFx        Tx   d 1 FF\n"
                                      "   1.000000 1  ErrorFrame\n"
                                      "   1.500000 CANFD   1 Rx        456 1 0 9 12 00 01 02 03 04 05 06 07 08 09 0A 0B\n"
                                      "   2.000000 1  LIN 3c Rx d 2 01 02 checksum = 4a\n"
                                      "End TriggerBlock\n",
                                      TraceFileFormat::VectorAsc);
    REQUIRE(f.frames.size() == 5);
    CHECK(f.channels == std::vector<std::string>{"CH 1", "CH 2"});
    CHECK(f.frames[0].id == 0x123);
    CHECK(f.frames[0].data[1] == 0x22);
    CHECK(f.frames[1].id == 0x1ABCDEF);
    CHECK(has_flag(f.frames[1], bus_flag::extended));
    CHECK(has_flag(f.frames[1], bus_flag::tx));
    CHECK(f.frames[1].iface == 1);
    CHECK(f.frames[1].ts_ns == 500000000);
    CHECK(is_error_frame(f.frames[2]));
    CHECK(has_flag(f.frames[3], bus_flag::fd));
    CHECK(f.frames[3].len == 12);
    CHECK(f.frames[3].data[11] == 0x0B);
    CHECK(f.frames[3].iface == 0);
    CHECK(f.frames[4].type == BusType::LIN);
    CHECK(f.frames[4].id == 0x3C);
    CHECK(f.frames[4].len == 2);
}

TEST_CASE("PEAK TRC 2.1")
{
    // Values as python-can's can.TRCReader reads these lines (STARTTIME 45000.5 = 1678881600 s).
    const ReplayFile f = replay_parse(";$FILEVERSION=2.1\r\n"
                                      ";$STARTTIME=45000.5\r\n"
                                      ";$COLUMNS=N,O,T,B,I,d,R,L,D\r\n"
                                      ";   comment\r\n"
                                      "      1      1059.900 DT 1     0300 Rx -  7    00 00 00 00 04 00 00\r\n"
                                      "      2      1753.227 FB 2     0400 Tx -  9    01 02 03 04 05 06 07 08 09 0A 0B 0C\r\n"
                                      "      3      1900.000 RR 2     0123 Rx -  2\r\n"
                                      "      4      2000.000 ER 1     -    Rx -  5    04 00 08 00 00\r\n"
                                      "      5      2100.000 ST 1     -    Rx -  4    00 00 00 04\r\n",
                                      TraceFileFormat::Trc);
    REQUIRE(f.frames.size() == 4);
    CHECK(f.channels == std::vector<std::string>{"CH 1", "CH 2"});
    CHECK(f.frames[0].ts_ns == 1678881601059900000);
    CHECK(f.frames[0].id == 0x300);
    CHECK(f.frames[0].len == 7);
    CHECK(f.frames[0].data[4] == 0x04);
    CHECK(f.frames[1].iface == 1);
    CHECK(has_flag(f.frames[1], bus_flag::fd));
    CHECK(has_flag(f.frames[1], bus_flag::brs));
    CHECK(has_flag(f.frames[1], bus_flag::tx));
    CHECK(f.frames[1].len == 12);
    CHECK(f.frames[1].data[11] == 0x0C);
    CHECK(has_flag(f.frames[2], bus_flag::rtr));
    CHECK(f.frames[2].len == 2);
    CHECK(is_error_frame(f.frames[3]));
    CHECK(f.frames[3].iface == 0);
}

TEST_CASE("pcap, LINKTYPE_CAN_SOCKETCAN")
{
    std::string d;
    le32(d, 0xA1B2C3D4);
    le16(d, 2);
    le16(d, 4);
    le32(d, 0);
    le32(d, 0);
    le32(d, 65535);
    le32(d, 227);
    const std::string frame = socketcan_frame(0x80001234, {1, 2, 3});
    le32(d, 10);
    le32(d, 500);
    le32(d, 16);
    le32(d, 16);
    d += frame;
    const ReplayFile f = replay_parse(d, TraceFileFormat::Pcap);
    REQUIRE(f.frames.size() == 1);
    CHECK(f.frames[0].id == 0x1234);
    CHECK(has_flag(f.frames[0], bus_flag::extended));
    CHECK(f.frames[0].len == 3);
    CHECK(f.frames[0].data[2] == 3);
    CHECK(f.frames[0].ts_ns == 10000500000);
}

TEST_CASE("pcapng with if_name")
{
    std::string d;
    // SHB
    le32(d, 0x0A0D0D0A);
    le32(d, 28);
    le32(d, 0x1A2B3C4D);
    le16(d, 1);
    le16(d, 0);
    le32(d, 0xFFFFFFFF);
    le32(d, 0xFFFFFFFF);
    le32(d, 28);
    // IDB: if_name "vcan0" (padded to 8) + opt_endofopt
    le32(d, 1);
    le32(d, 36);
    le16(d, 227);
    le16(d, 0);
    le32(d, 0);
    le16(d, 2);
    le16(d, 5);
    d += std::string("vcan0\0\0\0", 8);
    le32(d, 0);
    le32(d, 36);
    // EPB, microseconds
    le32(d, 6);
    le32(d, 48);
    le32(d, 0);
    le32(d, 0);
    le32(d, 2000000);
    le32(d, 16);
    le32(d, 16);
    d += socketcan_frame(0x7FF, {0xAA});
    le32(d, 48);
    const ReplayFile f = replay_parse(d, TraceFileFormat::PcapNg);
    REQUIRE(f.frames.size() == 1);
    CHECK(f.channels == std::vector<std::string>{"vcan0"});
    CHECK(f.frames[0].id == 0x7FF);
    CHECK(f.frames[0].data[0] == 0xAA);
    CHECK(f.frames[0].ts_ns == 2000000000);
}

TEST_CASE("filter rows and plan")
{
    const ReplayFile f = replay_parse("(10.000000) a 100#01\n"
                                      "(10.100000) a 200#02\n"
                                      "(10.200000) b 100#03\n"
                                      "(10.300000) a 100#04\n"
                                      "(10.400000) a 20000004#0000000000000000\n",
                                      TraceFileFormat::CanDump);
    std::vector<ReplayIdRow> rows = replay_id_rows(f);
    REQUIRE(rows.size() == 4); // a:100, a:200, a:error, b:100
    CHECK(rows[0].count == 2);
    CHECK(rows[2].id == replay_error_id);
    CHECK(rows[3].channel == 1);
    rows[1].rx_on = false; // a:200 off

    const std::vector<ReplayStep> plan = replay_plan(f, rows, {3, replay_trace_only});
    REQUIRE(plan.size() == 4);
    CHECK(plan[0].at_ns == 0);
    CHECK(plan[0].target == 3);
    CHECK(plan[1].at_ns == 200000000);
    CHECK(plan[1].target == replay_trace_only); // channel b unmapped
    CHECK(plan[2].msg.data[0] == 4);
    CHECK(plan[3].target == replay_trace_only); // error frames are never sent
}

TEST_CASE("replay a candump file onto vcan0 with its timing")
{
    if (if_nametoindex("vcan0") == 0)
    {
        MESSAGE("vcan0 not up, skipped");
        return;
    }
    std::deque<Iface> ifaces;
    auto& i = ifaces.emplace_back();
    i.ops = &socketcan_driver;
    i.info.name = "vcan0";
    i.info.details = "vcan"; // as enumerate: never `ip link set` (pkexec) the shared vcan
    Setup setup;
    setup.networks.push_back({.interfaces = {{.driver = "SocketCAN", .name = "vcan0"}}});
    REQUIRE(ifaces_start(ifaces, setup, {}, nullptr) == 1);

    const int peer = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    REQUIRE(peer >= 0);
    timeval tv{.tv_sec = 1, .tv_usec = 0};
    setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = static_cast<int>(if_nametoindex("vcan0"));
    REQUIRE(bind(peer, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    // vcan0 is shared with other runs: tag the frames with our pid.
    const auto pid = static_cast<uint32_t>(getpid());
    const std::string tag = std::format("{:08X}", pid);
    const std::string text = std::format("(100.000000) vcan0 5A1#{0}01\n"
                                         "(100.050000) vcan0 5A1#{0}02\n"
                                         "(100.150000) vcan0 5A1#{0}03\n",
                                         tag);
    Replay r;
    r.data.file = replay_parse(text, TraceFileFormat::CanDump);
    r.data.rows = replay_id_rows(r.data.file);
    r.mapping = {0};
    Tasks tasks;
    const auto start = std::chrono::steady_clock::now();
    replay_start(r, ifaces, tasks);

    std::vector<std::pair<uint8_t, std::chrono::steady_clock::duration>> got;
    while (got.size() < 3)
    {
        can_frame fr{};
        if (read(peer, &fr, sizeof(fr)) != CAN_MTU)
        {
            break;
        }
        if (fr.can_id == 0x5A1 && fr.len == 5 && std::format("{:02X}{:02X}{:02X}{:02X}", fr.data[0], fr.data[1], fr.data[2], fr.data[3]) == tag)
        {
            got.emplace_back(fr.data[4], std::chrono::steady_clock::now() - start);
        }
    }
    close(peer);
    REQUIRE(got.size() == 3);
    CHECK(got[0].first == 1);
    CHECK(got[2].first == 3);
    // 150 ms file time; allow scheduling slack under the sanitizers.
    CHECK(got[2].second - got[0].second >= std::chrono::milliseconds(140));
    CHECK(got[2].second - got[0].second < std::chrono::milliseconds(400));
    for (int t = 0; t < 100 && r.running; ++t)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(!r.running);
    CHECK(r.position == 3);
    CHECK(tasks.queue.empty()); // everything went out on the interface
    replay_stop(r);
    ifaces_stop(ifaces);
}

TEST_CASE("stop interrupts a long wait at once")
{
    Replay r;
    r.data.file = replay_parse("(0.0) x 1#01\n(60.0) x 1#02\n", TraceFileFormat::CanDump);
    r.data.rows = replay_id_rows(r.data.file);
    r.mapping = {replay_trace_only};
    std::deque<Iface> ifaces;
    Tasks tasks;
    replay_start(r, ifaces, tasks);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto t = std::chrono::steady_clock::now();
    replay_stop(r);
    CHECK(std::chrono::steady_clock::now() - t < std::chrono::milliseconds(500));
    CHECK(r.position == 1);
    CHECK(tasks.queue.size() == 1); // trace-only frame posted to the main thread
}

namespace
{

// 300k candump lines on two channels, in a temp file; returns the text too.
std::string write_big_candump(const std::filesystem::path& path)
{
    std::string text;
    for (int i = 0; i < 300000; ++i)
    {
        text += std::format("({}.{:06}) {} {:03X}#{:02X}{:02X}0102\n", 1000 + i / 1000, (i % 1000) * 1000,
                            i % 3 == 0 ? "vcanX" : "vcanY", 0x100 + i % 50, i & 0xFF, (i >> 8) & 0xFF);
    }
    std::ofstream(path, std::ios::binary) << text;
    return text;
}

// Unique across processes: getpid() alone collides between PID-namespaced sandboxes that
// share /tmp (every run is pid 2 or 3 there), and one run then truncates the other's file.
std::filesystem::path temp_trace(std::string_view stem)
{
    return std::filesystem::temp_directory_path()
        / std::format("{}_{}_{:08x}{:08x}.candump", stem, getpid(), std::random_device{}(), std::random_device{}());
}

bool same_frame(const BusMessage& a, const BusMessage& b)
{
    return a.id == b.id && a.flags == b.flags && a.errors == b.errors && a.iface == b.iface && a.len == b.len
        && a.dlc == b.dlc && a.type == b.type && a.ts_ns == b.ts_ns && a.data == b.data;
}

} // namespace

TEST_CASE("replay_load parses a big file on the loader thread")
{
    const auto path = temp_trace("replay_big");
    const std::string text = write_big_candump(path);
    const ReplayFile sync = replay_parse(text, TraceFileFormat::CanDump);
    REQUIRE(sync.frames.size() == 300000);

    App app;
    Replay r;
    r.open = true;
    const auto t0 = std::chrono::steady_clock::now();
    replay_load(app, r, path.string());
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(100)); // returns at once
    CHECK(r.loader.joinable());
    CHECK(r.data.file.frames.empty());
    float last = 0.0f;
    bool monotonic = true;
    while (!replay_load_poll(app, r) && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60))
    {
        const float f = r.load_fraction;
        monotonic = monotonic && f >= last;
        last = f;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(monotonic);
    CHECK(r.load_fraction == 1.0f);
    CHECK(!r.loader.joinable());
    CHECK(r.data.path == path.string());
    REQUIRE(r.data.file.frames.size() == sync.frames.size());
    CHECK(r.data.file.channels == sync.channels);
    CHECK(std::ranges::equal(r.data.file.frames, sync.frames, same_frame));
    CHECK(r.data.rows.size() == 100);
    CHECK(r.mapping == std::vector<int>{replay_trace_only, replay_trace_only});
    CHECK(r.data.channel_lin == std::vector<char>{0, 0});
    CHECK(r.data.info.find("Messages: 300000") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("replay_load: cancel, restart and destroy mid-load leave no threads")
{
    const auto path = temp_trace("replay_cancel");
    write_big_candump(path);
    App app;
    {
        Replay r;
        replay_load(app, r, path.string());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const auto t = std::chrono::steady_clock::now();
        replay_load_cancel(r);
        CHECK(std::chrono::steady_clock::now() - t < std::chrono::milliseconds(500));
        CHECK(!r.loader.joinable());
        CHECK(!replay_load_poll(app, r));
        CHECK(r.data.file.frames.empty());

        // A new load cancels the running one; only the second result arrives.
        replay_load(app, r, "/nonexistent/replay.candump");
        replay_load(app, r, path.string());
        while (!replay_load_poll(app, r))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(r.data.file.frames.size() == 300000);

        replay_load(app, r, path.string()); // destroyed while loading: the jthread joins
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    Replay missing;
    replay_load(app, missing, "/nonexistent/replay.candump");
    while (!replay_load_poll(app, missing))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(missing.data.info == "Error: Cannot open file.");
    CHECK(missing.data.path.empty());
    std::filesystem::remove(path);
}

TEST_CASE("closing the Replay window stops the playback (T87b a3 F4)")
{
    App app;
    app.workspace.tabs.push_back({.uid = 7});
    Replay r;
    r.data.file = replay_parse("(0.0) x 1#01\n(60.0) x 1#02\n", TraceFileFormat::CanDump);
    r.data.rows = replay_id_rows(r.data.file);
    r.mapping = {replay_trace_only};
    replay_start(r, app.ifaces, app.tasks);
    REQUIRE(r.running);
    r.open = false; // the window's X
    draw_replay(app, app.workspace.tabs[0], r);
    CHECK_FALSE(r.running);
    CHECK_FALSE(r.player.joinable());
}

namespace
{

void replay_frame(App& app, Replay& r)
{
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({900, 600});
    draw_replay(app, app.workspace.tabs[0], r);
    ImGui::EndFrame();
}

} // namespace

TEST_CASE("the channel checkbox in the filter table takes the click, not the tree node (T87b a3 F5)")
{
    UiTest ctx({1000, 700});
    App app;
    app.workspace.tabs.push_back({.uid = 8});
    Replay r;
    r.open = true;
    r.data.file = replay_parse("(0.0) a 100#01\n(0.1) a 200#02\n", TraceFileFormat::CanDump);
    r.data.rows = replay_id_rows(r.data.file);
    r.data.channel_lin = {0};
    r.mapping = {replay_trace_only};
    REQUIRE(r.data.rows.size() == 2);
    REQUIRE(r.data.rows[0].rx_on);
    replay_frame(app, r);
    replay_frame(app, r);
    ImGuiWindow* w = ImGui::FindWindowByName(workspace_window_name(app.workspace.tabs[0], "Replay").c_str());
    REQUIRE(w != nullptr);
    ImGuiTable* t = ImGui::TableFindByID(w->GetID("##filter"));
    REQUIRE(t != nullptr);
    const ImGuiStyle& st = ImGui::GetStyle();
    // On the label of the channel checkbox ("a (CAN)"), right of the tree arrow, in the first body row.
    const ImVec2 at{t->Columns[0].MinX + st.CellPadding.x + ImGui::GetTreeNodeToLabelSpacing() + st.ItemSpacing.x
                        + ImGui::GetFrameHeight() + 12.0f,
                    t->OuterRect.Min.y + ImGui::GetTextLineHeight() + st.CellPadding.y * 3.0f + ImGui::GetFrameHeight() * 0.5f};
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(at.x, at.y);
    replay_frame(app, r);
    io.AddMouseButtonEvent(0, true);
    replay_frame(app, r);
    io.AddMouseButtonEvent(0, false);
    replay_frame(app, r);
    CHECK_FALSE(r.data.rows[0].rx_on); // unticked the whole channel
    CHECK_FALSE(r.data.rows[1].rx_on);
}
