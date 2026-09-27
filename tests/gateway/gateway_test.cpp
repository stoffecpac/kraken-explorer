// ui/gateway: rule matching, and forwarding vcan0 -> vcan1 through the RX consumer
// (skipped when vcan0/vcan1 are not up).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstring>

#include <imgui.h>
#include <imgui_internal.h>
#include <pugixml.hpp>

#include "app.h"
#include "ui/gateway.h"
#include "ui_test.h"

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

extern const DriverOps socketcan_driver;

TEST_CASE("gateway_matches: source, id, id type; RX data frames only")
{
    const GatewayRule rule{.id = 0x123, .src = 1, .dst = 2};
    const BusMessage m{.id = 0x123, .iface = 1};
    CHECK(gateway_matches(rule, m));
    CHECK(!gateway_matches(rule, BusMessage{.id = 0x124, .iface = 1}));
    CHECK(!gateway_matches(rule, BusMessage{.id = 0x123, .iface = 2}));
    CHECK(!gateway_matches(rule, BusMessage{.id = 0x123, .flags = bus_flag::extended, .iface = 1}));
    CHECK(!gateway_matches(rule, BusMessage{.id = 0x123, .flags = bus_flag::tx, .iface = 1})); // own TX echo
    CHECK(!gateway_matches(rule, BusMessage{.id = 0x123, .errors = bus_error::ack, .iface = 1}));
    CHECK(!gateway_matches(rule, BusMessage{.id = 0x123, .iface = 1, .type = BusType::LIN}));
    const GatewayRule ext{.id = 0x123, .extended = true, .src = 1, .dst = 2};
    CHECK(gateway_matches(ext, BusMessage{.id = 0x123, .flags = bus_flag::extended, .iface = 1}));
}

TEST_CASE("gateway_rule_problem: same/missing interface, reverse of an existing rule")
{
    Gateway gw;
    CHECK(gateway_rule_problem(gw, UINT16_MAX, 1) == GatewayRuleProblem::SameInterface);
    CHECK(gateway_rule_problem(gw, 0, UINT16_MAX) == GatewayRuleProblem::SameInterface);
    CHECK(gateway_rule_problem(gw, 2, 2) == GatewayRuleProblem::SameInterface);
    CHECK(gateway_rule_problem(gw, 0, 1) == GatewayRuleProblem::None);
    gw.rules.push_back({.id = 1, .src = 0, .dst = 1});
    CHECK(gateway_rule_problem(gw, 1, 0) == GatewayRuleProblem::Reverse);
    CHECK(gateway_rule_problem(gw, 0, 1) == GatewayRuleProblem::None); // same direction again
    CHECK(gateway_rule_problem(gw, 1, 2) == GatewayRuleProblem::None);
    CHECK(gateway_rule_problem(gw, 1, 1) == GatewayRuleProblem::SameInterface);
}

TEST_CASE("rule row edit refuses source == destination (T87b a2 F5: frames echoed onto their own bus)")
{
    Gateway gw;
    gw.rules.push_back({.id = 1, .src = 0, .dst = 1});
    CHECK_FALSE(gateway_rule_reroute(gw, 0, 1, 1));
    CHECK_FALSE(gateway_rule_reroute(gw, 0, UINT16_MAX, 1));
    CHECK_FALSE(gateway_rule_reroute(gw, 1, 2, 3)); // no such rule
    CHECK(gw.rules[0].src == 0);
    CHECK(gw.rules[0].dst == 1);
    CHECK(gateway_rule_reroute(gw, 0, 2, 1));
    CHECK(gw.rules[0].src == 2);
    CHECK(gw.rules[0].dst == 1);
}

TEST_CASE("disabled gateway forwards nothing")
{
    std::deque<Iface> ifaces(2);
    Gateway gw;
    gw.ifaces = &ifaces;
    gw.rules.push_back({.id = 1, .src = 0, .dst = 1});
    gateway_rx_consumer(&gw, BusMessage{.id = 1});
    CHECK(gw.forwarded == 0);
    CHECK(gw.failed == 0);
    gw.enabled = true;
    gateway_rx_consumer(&gw, BusMessage{.id = 1});
    CHECK(gw.failed == 1); // destination closed
}

TEST_CASE("workspace XML: rules by driver + interface name and the enabled state survive a reorder (T87c)")
{
    static const DriverOps sc{.name = "SocketCAN"};
    const auto make = [](std::initializer_list<const char*> names)
    {
        std::deque<Iface> ifaces(names.size());
        uint16_t n = 0;
        for (const char* name : names)
        {
            ifaces[n].ops = &sc;
            ifaces[n].index = n;
            ifaces[n++].info.name = name;
        }
        return ifaces;
    };
    const std::deque<Iface> saved = make({"vcan0", "vcan1", "vcan2", "vcan3"});
    Gateway a;
    a.enabled = true;
    a.rules = {{.id = 0x123, .name = "Ping", .src = 0, .dst = 1},
               {.id = 0x18FEEE00, .extended = true, .src = 2, .dst = 0},
               {.id = 0x7, .src = 3, .dst = 0}}; // vcan3 is gone at load
    pugi::xml_document doc;
    pugi::xml_node root = doc.append_child("kraken-workspace");
    gateway_save_xml(a, saved, root);

    const std::deque<Iface> now = make({"vcan2", "vcan0", "vcan1"});
    Gateway b;
    gateway_load_xml(b, now, root);
    CHECK(b.enabled);
    REQUIRE(b.rules.size() == 2);
    CHECK(b.rules[0].id == 0x123);
    CHECK(b.rules[0].name == "Ping");
    CHECK_FALSE(b.rules[0].extended);
    CHECK(b.rules[0].src == 1); // vcan0
    CHECK(b.rules[0].dst == 2); // vcan1
    CHECK(b.rules[1].id == 0x18FEEE00);
    CHECK(b.rules[1].extended);
    CHECK(b.rules[1].src == 0); // vcan2
    CHECK(b.rules[1].dst == 1);

    // A workspace without <gateway> clears the rules; nothing to keep writes no element.
    gateway_load_xml(b, now, pugi::xml_document().append_child("kraken-workspace"));
    CHECK(b.rules.empty());
    CHECK_FALSE(b.enabled);
    pugi::xml_document empty;
    gateway_save_xml(b, now, empty.append_child("kraken-workspace"));
    CHECK_FALSE(empty.first_child().child("gateway"));
}

TEST_CASE("a disabled gateway's rules can still be removed; enabling resets the counters (T87c)")
{
    UiTest ctx;
    App app;
    Gateway& gw = app.gateway;
    gw.open = true;
    gw.rules.push_back({.id = 1, .src = 0, .dst = 1});
    gw.selected = 0;
    gw.forwarded = 5;
    gw.failed = 2;
    ImGuiID remove = 0;
    ImGuiID enable = 0;
    const auto draw = [&]
    {
        ImGui::NewFrame();
        draw_gateway(app, gw);
        ImGui::Begin("CAN Gateway");
        remove = ImGui::GetID("Remove");
        enable = ImGui::GetID("Enable Gateway");
        ImGui::End();
        ImGui::EndFrame();
    };
    draw();
    ImGui::ActivateItemByID(remove);
    for (int i = 0; i < 3; ++i)
    {
        draw();
    }
    CHECK(gw.rules.empty());
    CHECK_FALSE(gw.enabled);
    ImGui::ActivateItemByID(enable);
    for (int i = 0; i < 3; ++i)
    {
        draw();
    }
    CHECK(gw.enabled);
    CHECK(gw.forwarded == 0);
    CHECK(gw.failed == 0);
}

namespace
{

int raw_socket(const char* name)
{
    const int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    timeval tv{.tv_sec = 1, .tv_usec = 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = static_cast<int>(if_nametoindex(name));
    return bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 ? s : -1;
}

} // namespace

TEST_CASE("forwards vcan0 -> vcan1")
{
    if (if_nametoindex("vcan0") == 0 || if_nametoindex("vcan1") == 0)
    {
        MESSAGE("vcan0/vcan1 not up, skipped");
        return;
    }
    std::deque<Iface> ifaces(2);
    for (uint16_t n = 0; n < 2; ++n)
    {
        ifaces[n].ops = &socketcan_driver;
        ifaces[n].index = n;
        ifaces[n].info.name = n == 0 ? "vcan0" : "vcan1";
        ifaces[n].info.details = "vcan"; // as enumerate: never `ip link set` (pkexec) the shared vcan
    }
    Setup setup;
    setup.networks.push_back({.interfaces = {{.driver = "SocketCAN", .name = "vcan0"}, {.driver = "SocketCAN", .name = "vcan1"}}});

    // vcan is shared with other runs: a pid-tagged id per run.
    const uint32_t id = 0x1000000u | static_cast<uint32_t>(getpid() & 0xFFFFF);
    Gateway gw;
    gw.ifaces = &ifaces;
    gw.enabled = true;
    gw.rules.push_back({.id = id, .extended = true, .src = 0, .dst = 1});
    const RxConsumer consumers[] = {{.fn = gateway_rx_consumer, .user = &gw}};
    REQUIRE(ifaces_start(ifaces, setup, consumers, nullptr) == 2);

    const int peer0 = raw_socket("vcan0");
    const int peer1 = raw_socket("vcan1");
    REQUIRE(peer0 >= 0);
    REQUIRE(peer1 >= 0);
    for (uint8_t k = 0; k < 3; ++k)
    {
        can_frame fr{};
        fr.can_id = id | CAN_EFF_FLAG;
        fr.len = 2;
        fr.data[0] = 0xA5;
        fr.data[1] = k;
        REQUIRE(write(peer0, &fr, sizeof(fr)) == CAN_MTU);
    }
    can_frame other{};
    other.can_id = (id + 1) | CAN_EFF_FLAG; // no rule: stays on vcan0
    REQUIRE(write(peer0, &other, sizeof(other)) == CAN_MTU);

    // This machine bridges vcan0 <-> vcan1 with kernel cangw rules (`cangw -L`), so vcan1 sees
    // every vcan0 frame anyway, as RX. Frames sent by a socket on this host, i.e. our gateway's
    // iface_send on vcan1, carry MSG_DONTROUTE (candump -x shows them as "TX"): count only those.
    // With the bridge our forwards also come back to vcan0 and would be forwarded again (a loop,
    // as with the Qt gateway), so the gateway is switched off once the three have been seen.
    unsigned got = 0; // bit k: frame k seen (the loop can repeat and reorder them)
    int stray = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && got != 7)
    {
        can_frame fr{};
        iovec iov{.iov_base = &fr, .iov_len = sizeof(fr)};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        if (recvmsg(peer1, &msg, 0) != CAN_MTU)
        {
            break;
        }
        if ((msg.msg_flags & MSG_DONTROUTE) == 0)
        {
            continue; // not sent on vcan1 by this host (e.g. the kernel bridge's copy)
        }
        if (fr.can_id == (id | CAN_EFF_FLAG) && fr.len == 2 && fr.data[0] == 0xA5)
        {
            got |= fr.data[1] < 3 ? 1u << fr.data[1] : 8u;
        }
        stray += fr.can_id == ((id + 1) | CAN_EFF_FLAG) ? 1 : 0;
    }
    {
        std::scoped_lock lock(gw.mutex);
        gw.enabled = false;
    }
    CHECK(got == 7);
    ifaces_stop(ifaces);
    CHECK(stray == 0);
    CHECK(gw.forwarded >= 3);
    close(peer0);
    close(peer1);
}
