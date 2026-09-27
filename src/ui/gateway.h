#pragma once

// CAN Gateway: forwards received frames with a given id from one interface to another.
// The forwarding runs on the RX threads (App::rx_consumers), not in the UI.

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "core/bus_message.h"

struct App;
struct Iface;

struct GatewayRule
{
    uint32_t id = 0;
    bool extended = false;
    std::string name; // DBC message name or empty
    uint16_t src = 0; // App::ifaces indices
    uint16_t dst = 0;
};

// A received (not TX, not error) CAN frame on the rule's source with the rule's id.
[[nodiscard]] bool gateway_matches(const GatewayRule& rule, const BusMessage& m) noexcept;

struct Gateway
{
    // Shared with the RX threads.
    std::mutex mutex;
    bool enabled = false;
    std::vector<GatewayRule> rules;
    std::deque<Iface>* ifaces = nullptr;
    std::atomic<uint64_t> forwarded{0};
    std::atomic<uint64_t> failed{0};

    // UI (main thread only)
    bool open = false;
    uint32_t id = 0;
    bool extended = false;
    std::string name;
    uint16_t src = UINT16_MAX; // App::ifaces index, UINT16_MAX = none
    uint16_t dst = UINT16_MAX;
    int selected = -1;
};

enum class GatewayRuleProblem
{
    None,
    SameInterface, // src == dst (or an interface missing): Add is disabled
    Reverse,       // dst -> src already exists: may flood both buses, warned but allowed
};

// Checks a new src -> dst rule (App::ifaces indices, UINT16_MAX = none) against gw.rules. Main
// thread only (rules are only written by the UI, so no lock).
[[nodiscard]] GatewayRuleProblem gateway_rule_problem(const Gateway& gw, uint16_t src, uint16_t dst) noexcept;

// RxConsumer::fn with user = Gateway*: sends every matching frame on the rule's destination.
void gateway_rx_consumer(void* user, const BusMessage& m);

// The "CAN Gateway" window while gw.open.
void draw_gateway(App& app, Gateway& gw);
