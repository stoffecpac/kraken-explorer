/*
  Copyright (c) 2024 - 2026 Schildkroet

  This file is part of cangaroo.

  cangaroo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  cangaroo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with cangaroo.  If not, see <http://www.gnu.org/licenses/>.
*/

// GrIP (CANIL adapter, 1a86:55d3) over core/serial. One GripDevice per serial port owns
// a worker jthread that decodes the GrIP byte stream into per-channel queues; every CAN
// and LIN channel of the device is an Iface of grip_driver sharing that device.
// GPIO reports and late channel capabilities reach the main thread through core/tasks.

#pragma once

#include <array>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/bus_message.h"
#include "core/serial.h"
#include "drivers/driver.h"

struct Tasks;

extern const DriverOps grip_driver;

// GPIO reports go to GripDevice::gpio and late capability changes to the matching
// Iface::info, both as tasks on the main thread. Call once at startup, before any device
// opens (app_init_interfaces); until then they are dropped.
void grip_attach(Tasks& tasks, std::deque<Iface>& ifaces);

// Serial port of the CANIL, set by enumerate (first 1a86:55d3 found); open() connects there.
// Main thread only.
extern std::string grip_port;

// ---------------------------------------------------------------------------
// Wire codec: <SOH> hex(header, 8 bytes) [<SOT> hex(payload)] <EOT>
// ---------------------------------------------------------------------------

namespace grip
{
inline constexpr uint8_t version = 4;          // GrIP protocol version in every header
inline constexpr std::size_t max_payload = 256;

enum MsgType : uint8_t
{
    msg_system_cmd = 0,
    msg_data = 2,
    msg_data_no_response = 3,
    msg_notification = 4,
    msg_response = 5,
    msg_error = 6,
    msg_sync = 7,
    msg_max = 8,
};

enum Ret : uint8_t
{
    ret_ok = 0,
    ret_wrong_param = 5,
};

// CRC-8/SAE-J1850 (poly 0x1D, init 0xFF, xorout 0xFF, not reflected).
[[nodiscard]] constexpr uint8_t crc8(std::span<const uint8_t> data) noexcept
{
    uint8_t crc = 0xFF;
    for (uint8_t b : data)
    {
        crc ^= b;
        for (int i = 0; i < 8; ++i)
        {
            crc = static_cast<uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x1D : crc << 1);
        }
    }
    return crc ^ 0xFF;
}
} // namespace grip

struct GripPacket
{
    uint8_t msg_type = 0;
    uint16_t length = 0;
    std::array<uint8_t, grip::max_payload> data{};
};

// Receive state machine. grip_parse() appends complete packets to `packets` and the
// return codes the host has to acknowledge (MSG_RESPONSE) to `acks`; the caller drains both.
struct GripParser
{
    enum class State : uint8_t
    {
        Idle,
        Header,
        WaitSot,
        Data,
    };
    State state = State::Idle;
    std::array<char, 16> hex{};      // header hex digits / one payload hex pair
    std::size_t count = 0;           // hex digits (Header, Data) collected so far
    GripPacket pkt;
    uint8_t crc_data = 0;
    uint32_t crc_errors = 0;
    std::vector<GripPacket> packets;
    std::vector<uint8_t> acks;
};

// Appends one encoded packet to out. False (nothing appended) if payload > 256 bytes.
bool grip_encode(std::string& out, uint8_t msg_type, uint8_t ret, std::span<const uint8_t> payload);
void grip_parse(GripParser& p, std::span<const uint8_t> bytes);

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

inline constexpr int grip_gpio_digital_pins = 16;
inline constexpr int grip_gpio_analog_pins = 8;     // pins 0..7 also report mV
inline constexpr int grip_gpio_max_cycle_ms = 255;  // cycle time is one byte on the wire

struct GripGpio
{
    uint16_t pins = 0;                                  // bit N = level of pin N
    std::array<uint16_t, grip_gpio_analog_pins> mv{};   // voltage per analog pin
    uint64_t reports = 0;                               // number of reports received
};

struct GripDeviceInfo
{
    std::string version;   // "major.minor-<build date>", empty until the device answered
    int can = 0;           // classic CAN channels
    int canfd = 0;         // CAN FD channels, numbered after the classic ones
    int lin = 0;
    int lin_tables = 0;    // LIN schedule tables per channel
};

struct GripTxPending
{
    uint8_t ch = 0;
    BusMessage msg;
    std::chrono::steady_clock::time_point sent;
    bool error_reported = false;
};

struct GripDevice
{
    std::string port_name;
    SerialPort port;
    std::mutex write_mutex;              // serialises serial_write (any thread)

    std::mutex mutex;                    // guards everything down to `failed`
    std::condition_variable cv;          // notified on every processed packet
    bool info_received = false;
    GripDeviceInfo info;
    std::vector<std::deque<BusMessage>> can_rx;   // index = CAN channel (classic, then FD)
    std::vector<std::deque<BusMessage>> lin_rx;
    std::vector<bool> can_enabled;
    std::vector<bool> lin_enabled;
    std::vector<uint8_t> can_state;      // GripCanState per channel
    std::vector<uint16_t> can_rx_drops;
    std::vector<uint8_t> lin_state;
    std::unordered_map<uint32_t, GripTxPending> tx_pending;   // key: correlation hash echoed by the device
    std::map<uint16_t, uint32_t> caps;   // key (bus_type << 8) | channel -> grip_cap bits
    bool failed = false;                 // serial error, worker stopped

    GripGpio gpio;                       // main thread only, updated through the tasks of grip_attach()

    std::jthread worker;                 // last member: joined before the rest is destroyed
};

// GrIP capability bits of DATA_CHANNEL_CAPABILITIES.
namespace grip_cap
{
inline constexpr uint32_t can_baud_10k = 1u << 0;
inline constexpr uint32_t can_baud_20k = 1u << 1;
inline constexpr uint32_t can_baud_50k = 1u << 2;
inline constexpr uint32_t can_baud_100k = 1u << 3;
inline constexpr uint32_t can_baud_125k = 1u << 4;
inline constexpr uint32_t can_baud_250k = 1u << 5;
inline constexpr uint32_t can_baud_500k = 1u << 6;
inline constexpr uint32_t can_baud_1m = 1u << 7;
inline constexpr uint32_t can_listen_only = 1u << 8;
inline constexpr uint32_t can_abom = 1u << 9;
inline constexpr uint32_t can_txecho = 1u << 10;
inline constexpr uint32_t can_fd = 1u << 11;
inline constexpr uint32_t lin_mode_master = 1u << 8;
inline constexpr uint32_t lin_mode_slave = 1u << 9;
}

enum GripCanState : uint8_t
{
    grip_can_off = 0,
    grip_can_stopped,
    grip_can_active,
    grip_can_error_warning,
    grip_can_error_passive,
};

// Opens the port (3 Mbaud), starts the worker and asks for the device info (waits up to
// 200 ms for it). Null if the port cannot be opened. The last shared_ptr closes it.
[[nodiscard]] std::shared_ptr<GripDevice> grip_device_open(const std::string& port_name);

// Asks for the capabilities of one channel (bus_type 0 = CAN, 1 = LIN) and waits up to
// timeout for them. Returns the last known bits, 0 if none arrived.
uint32_t grip_request_caps(GripDevice& dev, uint8_t bus_type, uint8_t ch, std::chrono::milliseconds timeout);

// The device of the running measurement (GpioControl window), null when none is open.
[[nodiscard]] std::shared_ptr<GripDevice> grip_open_device();

void grip_gpio_config(GripDevice& dev, bool enable, uint8_t cycle_ms, uint16_t dir_mask);
void grip_gpio_output(GripDevice& dev, uint16_t mask);

// Setup-dialog view of a channel from its GrIP capability bits (0 = unknown: defaults).
void grip_apply_caps(IfaceInfo& info, bool lin, bool fd_channel, uint32_t caps);
