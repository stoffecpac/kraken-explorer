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

// GrIP-CANIL driver. Replaces GrIPDriver, GrIPInterface, GrIPHandler, GrIP.cpp and CRC.c.
// Every multi-byte field on the wire is little-endian (packed structs on the STM32).

#include "drivers/grip.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>

#include "core/log.h"
#include "core/tasks.h"
#include "db/model/lin_db.h"
#include "drivers/slcan_codec.h"

std::string grip_port;

namespace
{

using namespace std::chrono_literals;

constexpr uint16_t grip_vid = 0x1A86;
constexpr uint16_t grip_pid = 0x55D3;
constexpr int grip_baud = 3000000;

constexpr uint8_t soh = 0x01;
constexpr uint8_t sot = 0x02;
constexpr uint8_t eot = 0x03;

// System command ids (Protocol_SystemHeader_t::Command)
constexpr uint8_t sys_report_info = 0;
constexpr uint8_t sys_send_can_cfg = 20;
constexpr uint8_t sys_send_lin_cfg = 21;
constexpr uint8_t sys_start_can = 22;
constexpr uint8_t sys_start_lin = 23;
constexpr uint8_t sys_add_lin_frame = 25;
constexpr uint8_t sys_lin_set_table = 28;
constexpr uint8_t sys_send_can_frame = 30;
constexpr uint8_t sys_set_lin_data = 31;
constexpr uint8_t sys_send_gpio_cfg = 32;
constexpr uint8_t sys_set_gpio_output = 33;
constexpr uint8_t sys_get_channel_caps = 34;
constexpr uint8_t sys_send_canfd_cfg = 35;
constexpr uint8_t sys_lin_sleep_wakeup = 36;
constexpr uint8_t sys_lin_diag_req = 37;
constexpr uint8_t sys_header_version = 1;

// Data report ids (MSG_DATA sub-command)
constexpr uint8_t data_can_msg = 254;
constexpr uint8_t data_lin_msg = 253;
constexpr uint8_t data_gpio = 252;
constexpr uint8_t data_channel_caps = 251;
constexpr uint8_t data_can_status = 220;
constexpr uint8_t data_lin_status = 219;
constexpr uint8_t data_can_tx_echo = 209;

constexpr uint8_t can_flag_ext = 0x01;
constexpr uint8_t can_flag_fd = 0x02;
constexpr uint8_t can_flag_rtr = 0x04;
constexpr uint8_t can_flag_brs = 0x08;

constexpr uint8_t lin_flag_responded = 0x01;
constexpr uint8_t lin_flag_valid_checksum = 0x02;
constexpr uint8_t lin_flag_sleep = 0x04;
constexpr uint8_t lin_flag_wakeup = 0x08;
constexpr uint8_t lin_flag_sporadic = 0x10;

// ---------------------------------------------------------------------------
// Byte helpers
// ---------------------------------------------------------------------------

void append_hex(std::string& out, std::span<const uint8_t> bytes)
{
    for (uint8_t b : bytes)
    {
        out += slcan::hex_nibble(b >> 4);
        out += slcan::hex_nibble(b & 0x0F);
    }
}

uint16_t get16(std::span<const uint8_t> d, std::size_t at) noexcept
{
    return static_cast<uint16_t>(d[at] | d[at + 1] << 8);
}

uint32_t get32(std::span<const uint8_t> d, std::size_t at) noexcept
{
    return static_cast<uint32_t>(d[at]) | static_cast<uint32_t>(d[at + 1]) << 8 | static_cast<uint32_t>(d[at + 2]) << 16 |
           static_cast<uint32_t>(d[at + 3]) << 24;
}

// System command payload: 4-byte header {version, command, length, data} + fields.
struct Payload
{
    std::array<uint8_t, grip::max_payload> b{};
    std::size_t n = 0;
};

Payload sys_cmd(uint8_t cmd, uint8_t data = 0)
{
    Payload p;
    p.b[0] = sys_header_version;
    p.b[1] = cmd;
    p.b[3] = data;
    p.n = 4;
    return p;
}

void put8(Payload& p, uint8_t v) noexcept
{
    if (p.n < p.b.size())
    {
        p.b[p.n++] = v;
    }
}

void put16(Payload& p, uint16_t v) noexcept
{
    put8(p, static_cast<uint8_t>(v));
    put8(p, static_cast<uint8_t>(v >> 8));
}

void put32(Payload& p, uint32_t v) noexcept
{
    put16(p, static_cast<uint16_t>(v));
    put16(p, static_cast<uint16_t>(v >> 16));
}

bool write_packet(GripDevice& dev, uint8_t msg_type, uint8_t ret, std::span<const uint8_t> payload)
{
    std::string wire;
    if (!grip_encode(wire, msg_type, ret, payload))
    {
        return false;
    }
    std::lock_guard lock(dev.write_mutex);
    return serial_write(dev.port, wire.data(), wire.size());
}

bool send_cmd(GripDevice& dev, Payload p)
{
    p.b[2] = static_cast<uint8_t>(p.n - 4); // length of the fields after the header
    return write_packet(dev, grip::msg_system_cmd, grip::ret_ok, std::span(p.b.data(), p.n));
}

uint16_t error_bits(uint8_t ef) noexcept
{
    uint16_t e = 0;
    e |= (ef & 0x01) ? bus_error::stuff : 0;
    e |= (ef & 0x02) ? bus_error::form : 0;
    e |= (ef & 0x04) ? bus_error::ack : 0;
    e |= (ef & 0x18) ? bus_error::bit : 0;   // bit recessive / dominant
    e |= (ef & 0x20) ? bus_error::crc : 0;
    e |= (ef & 0x40) ? bus_error::generic : 0;
    return static_cast<uint16_t>(e);
}

// ---------------------------------------------------------------------------
// Channel naming: "CANIL-CAN<n>", "CANIL-CANFD<n>" (n counts classic + FD), "CANIL-LIN<n>"
// ---------------------------------------------------------------------------

struct ChannelId
{
    bool lin = false;
    bool fd = false;
    int ch = -1;
};

ChannelId parse_channel(std::string_view name)
{
    ChannelId id{.lin = name.starts_with("CANIL-LIN"), .fd = name.starts_with("CANIL-CANFD")};
    const auto digits = name.find_last_not_of("0123456789");
    if (name.starts_with("CANIL-") && digits != std::string_view::npos && digits + 1 < name.size())
    {
        std::from_chars(name.data() + digits + 1, name.data() + name.size(), id.ch);
    }
    return id;
}

// Main thread only.
std::weak_ptr<GripDevice> g_device;   // device of the running measurement
std::deque<Iface>* g_ifaces = nullptr;   // grip_attach(): where capability updates go
// Set once before any device opens, read by the workers.
Tasks* g_tasks = nullptr;

struct GripChannel
{
    std::shared_ptr<GripDevice> dev;
    uint8_t ch = 0;
    bool lin = false;
    std::atomic<uint64_t> rx_frames{0};
    std::atomic<uint64_t> tx_frames{0};
    std::atomic<uint64_t> tx_errors{0};
};

// Caller holds dev.mutex. Null when the device reported fewer channels.
std::deque<BusMessage>* rx_queue(GripDevice& dev, bool lin, uint8_t ch)
{
    auto& qs = lin ? dev.lin_rx : dev.can_rx;
    return ch < qs.size() ? &qs[ch] : nullptr;
}

// ---------------------------------------------------------------------------
// Worker: packets from the device (caller holds dev.mutex)
// ---------------------------------------------------------------------------

void process_info(GripDevice& dev, std::span<const uint8_t> d)
{
    auto& i = dev.info;
    const auto date = reinterpret_cast<const char*>(d.data() + 14);
    i.version = std::format("{}.{}-<{}>", d[4], d[5], std::string_view(date, strnlen(date, 128)));
    i.can = d[7];
    i.canfd = d[8];
    i.lin = d[9];
    i.lin_tables = d[12];
    const auto can = static_cast<std::size_t>(i.can + i.canfd);
    const auto lin = static_cast<std::size_t>(i.lin);
    dev.can_rx.assign(can, {});
    dev.can_enabled.assign(can, false);
    dev.can_state.assign(can, grip_can_off);
    dev.can_rx_drops.assign(can, 0);
    dev.lin_rx.assign(lin, {});
    dev.lin_enabled.assign(lin, false);
    dev.lin_state.assign(lin, grip_can_off);
    dev.info_received = true;
}

void process_can_frame(GripDevice& dev, std::span<const uint8_t> d, int64_t ts)
{
    // Channel, ID u32, DLC (bytes), Flags, ErrFlags, Time u32, Data[64]
    const uint8_t ch = d[4];
    const uint8_t flags = d[10];
    BusMessage m{.id = get32(d, 5), .errors = error_bits(d[11]), .type = BusType::CAN, .ts_ns = ts};
    m.flags |= (flags & can_flag_ext) ? bus_flag::extended : 0;
    m.flags |= (flags & can_flag_fd) ? bus_flag::fd : 0;
    m.flags |= (flags & can_flag_rtr) ? bus_flag::rtr : 0;
    m.flags |= (flags & can_flag_brs) ? bus_flag::brs : 0;
    set_length(m, d[9]);
    std::copy_n(d.begin() + 16, m.len, m.data.begin());
    if (ch < dev.can_enabled.size() && dev.can_enabled[ch])
    {
        dev.can_rx[ch].push_back(m);
    }
}

void process_lin_frame(GripDevice& dev, std::span<const uint8_t> d, int64_t ts)
{
    // Channel, ID, DLC, Direction (1 = subscriber response / RX), Delay, Flags, Time u32, Data[8]
    const uint8_t ch = d[4];
    const uint8_t flags = d[9];
    BusMessage m{.id = d[5], .type = BusType::LIN, .ts_ns = ts};
    if (flags & (lin_flag_sleep | lin_flag_wakeup))
    {
        m.flags |= (flags & lin_flag_sleep) ? bus_flag::lin_sleep : 0;
        m.flags |= (flags & lin_flag_wakeup) ? bus_flag::lin_wakeup : 0;
    }
    else if (!(flags & lin_flag_responded))
    {
        m.errors = bus_error::lin_not_responded;
    }
    else if (!(flags & lin_flag_valid_checksum))
    {
        m.errors = bus_error::lin_checksum_error;
    }
    m.flags |= d[7] == 1 ? 0 : bus_flag::tx;
    set_length(m, std::min<int>(d[6], 8));
    std::copy_n(d.begin() + 14, m.len, m.data.begin());
    if (ch < dev.lin_enabled.size() && dev.lin_enabled[ch])
    {
        dev.lin_rx[ch].push_back(m);
    }
}

void post_gpio(const std::weak_ptr<GripDevice>& self, std::span<const uint8_t> d)
{
    if (!g_tasks)
    {
        return;
    }
    GripGpio g{.pins = get16(d, 4)};
    for (int i = 0; i < grip_gpio_analog_pins; ++i)
    {
        g.mv[static_cast<std::size_t>(i)] = get16(d, 6 + 2 * static_cast<std::size_t>(i));
    }
    tasks_post(*g_tasks, [self, g](App&) {
        if (auto dev = self.lock())
        {
            dev->gpio = {.pins = g.pins, .mv = g.mv, .reports = dev->gpio.reports + 1};
        }
    });
}

// Channel capabilities that change after enumerate (or arrive late) update the Iface info.
void post_caps(bool lin, uint8_t ch, uint32_t caps)
{
    if (!g_tasks)
    {
        return;
    }
    tasks_post(*g_tasks, [lin, ch, caps](App&) {
        for (auto& i : *g_ifaces)
        {
            const ChannelId id = parse_channel(i.info.name);
            if (i.ops == &grip_driver && id.lin == lin && id.ch == ch)
            {
                grip_apply_caps(i.info, lin, id.fd, caps);
            }
        }
    });
}

void process_packet(GripDevice& dev, const GripPacket& pkt, int64_t ts, const std::weak_ptr<GripDevice>& self)
{
    const std::span<const uint8_t> d(pkt.data);
    const uint8_t cmd = d[1];
    if (pkt.msg_type == grip::msg_system_cmd && cmd == sys_report_info)
    {
        process_info(dev, d);
        return;
    }
    if (pkt.msg_type == grip::msg_notification)
    {
        // [0] = notification type, [1..] = text
        const auto text = reinterpret_cast<const char*>(d.data() + 1);
        log_debug(std::format("GrIP device: {}", std::string_view(text, strnlen(text, d.size() - 1))));
        return;
    }
    if (pkt.msg_type != grip::msg_data && pkt.msg_type != grip::msg_data_no_response)
    {
        return;
    }
    switch (cmd)
    {
    case data_can_msg:
        process_can_frame(dev, d, ts);
        break;
    case data_lin_msg:
        process_lin_frame(dev, d, ts);
        break;
    case data_can_status:   // BusState[8], RxDropCount u16[8]
        for (std::size_t i = 0; i < dev.can_state.size() && i < 8; ++i)
        {
            dev.can_state[i] = d[4 + i];
            dev.can_rx_drops[i] = get16(d, 12 + 2 * i);
        }
        break;
    case data_lin_status:
        for (std::size_t i = 0; i < dev.lin_state.size() && i < 8; ++i)
        {
            dev.lin_state[i] = d[4 + i];
        }
        break;
    case data_can_tx_echo:   // header data = error flags, Hash u32
        if (auto it = dev.tx_pending.find(get32(d, 4)); it != dev.tx_pending.end())
        {
            auto& e = it->second;
            e.msg.errors = error_bits(d[3]);
            if (e.ch < dev.can_rx.size())
            {
                dev.can_rx[e.ch].push_back(e.msg);
            }
            dev.tx_pending.erase(it);
        }
        break;
    case data_gpio:   // PinState u16, Voltage_mV u16[8]
        post_gpio(self, d);
        break;
    case data_channel_caps:   // BusType, Channel, Capabilities u32
    {
        const auto key = static_cast<uint16_t>(d[4] << 8 | d[5]);
        const uint32_t caps = get32(d, 6);
        const auto old = dev.caps.find(key);
        const bool changed = old != dev.caps.end() && old->second != caps;
        dev.caps[key] = caps;
        if (changed)
        {
            post_caps(d[4] == 1, d[5], caps);
        }
        break;
    }
    default:
        log_debug(std::format("GrIP: unknown data report {}", cmd));
        break;
    }
}

// TX frames without echo: reported as error after 1 s, forgotten after 5 s.
void purge_tx_pending(GripDevice& dev, std::chrono::steady_clock::time_point now)
{
    for (auto it = dev.tx_pending.begin(); it != dev.tx_pending.end();)
    {
        auto& e = it->second;
        const auto age = now - e.sent;
        if (age >= 5s)
        {
            it = dev.tx_pending.erase(it);
            continue;
        }
        if (age >= 1s && !e.error_reported)
        {
            e.error_reported = true;
            BusMessage m = e.msg;
            m.errors |= bus_error::tx_timeout;
            if (e.ch < dev.can_rx.size())
            {
                dev.can_rx[e.ch].push_back(m);
            }
        }
        ++it;
    }
}

void worker(std::stop_token stop, GripDevice& dev, std::weak_ptr<GripDevice> self)
{
    GripParser parser;
    std::array<uint8_t, 4096> buf;
    auto last_purge = std::chrono::steady_clock::now();
    while (!stop.stop_requested())
    {
        const long n = serial_read(dev.port, buf.data(), buf.size(), 20ms);
        if (n < 0)
        {
            log_error(std::format("GrIP {}: {}", dev.port_name, dev.port.error));
            std::lock_guard lock(dev.mutex);
            dev.failed = true;
            dev.cv.notify_all();
            return;
        }
        const int64_t ts = now_ns();
        grip_parse(parser, std::span(buf.data(), static_cast<std::size_t>(n)));
        for (uint8_t ret : parser.acks)
        {
            write_packet(dev, grip::msg_response, ret, {});
        }
        parser.acks.clear();

        const auto now = std::chrono::steady_clock::now();
        const bool purge = now - last_purge >= 100ms;
        if (parser.packets.empty() && !purge)
        {
            continue;
        }
        std::lock_guard lock(dev.mutex);
        for (const auto& pkt : parser.packets)
        {
            process_packet(dev, pkt, ts, self);
        }
        parser.packets.clear();
        if (purge)
        {
            purge_tx_pending(dev, now);
            last_purge = now;
        }
        dev.cv.notify_all();
    }
}

// ---------------------------------------------------------------------------
// Device commands
// ---------------------------------------------------------------------------

void can_enable(GripDevice& dev, uint8_t ch, bool enable)
{
    auto p = sys_cmd(sys_start_can);
    {
        std::lock_guard lock(dev.mutex);
        if (ch >= dev.can_enabled.size())
        {
            return;
        }
        dev.can_enabled[ch] = enable;
        if (!enable)
        {
            dev.can_rx[ch].clear();
        }
        // The firmware takes the state of both channels at once.
        put8(p, dev.can_enabled.size() > 0 && dev.can_enabled[0]);
        put8(p, dev.can_enabled.size() > 1 && dev.can_enabled[1]);
    }
    send_cmd(dev, p);
}

void lin_enable(GripDevice& dev, uint8_t ch, bool enable)
{
    {
        std::lock_guard lock(dev.mutex);
        if (ch >= dev.lin_enabled.size())
        {
            return;
        }
        dev.lin_enabled[ch] = enable;
        if (!enable)
        {
            dev.lin_rx[ch].clear();
        }
    }
    auto p = sys_cmd(sys_start_lin);
    put8(p, ch);
    put8(p, enable);
    send_cmd(dev, p);
}

void lin_set_table(GripDevice& dev, uint8_t ch, uint8_t table)
{
    auto p = sys_cmd(sys_lin_set_table);
    put8(p, ch);
    put8(p, table);
    send_cmd(dev, p);
}

void open_can(GripDevice& dev, uint8_t ch, uint32_t caps, const IfaceConfig& cfg)
{
    can_enable(dev, ch, false);
    std::this_thread::sleep_for(2ms);

    const uint32_t baud = cfg.is_custom_bitrate ? cfg.custom_bitrate : cfg.bitrate;
    const uint32_t arb = baud > 0 ? baud : 500000;
    const bool fd = (caps & grip_cap::can_fd) != 0;
    auto p = sys_cmd(fd ? sys_send_canfd_cfg : sys_send_can_cfg);
    put8(p, ch);
    put32(p, arb);
    if (fd)
    {
        const uint32_t data = cfg.is_custom_fd_bitrate ? cfg.custom_fd_bitrate : cfg.fd_bitrate;
        put32(p, data > 0 ? data : arb);
    }
    put8(p, 1);   // TX echo: TX frames come back through the 209 echo report
    put8(p, cfg.auto_restart);
    put8(p, cfg.listen_only);
    send_cmd(dev, p);

    std::this_thread::sleep_for(20ms);
    can_enable(dev, ch, true);
}

void open_lin(GripDevice& dev, uint8_t ch, int max_tables, const IfaceConfig& cfg)
{
    lin_enable(dev, ch, false);
    std::this_thread::sleep_for(2ms);
    if (cfg.lin_ldf_path.empty())
    {
        return; // as before: without an LDF the channel stays disabled
    }

    LinDb ldb;
    if (lin_db_load(ldb, cfg.lin_ldf_path))
    {
        const bool master = cfg.lin_node_mode == LinNodeMode::Master;
        const bool listen_only = cfg.lin_listen_only;
        const std::string& slave = cfg.lin_slave_node;
        const std::string diag_node = lin_db_diag_node(ldb, master, slave);
        const LinDiagTiming t = lin_db_diag_timing(ldb, diag_node);

        // Channel, Baudrate u16, Timebase, Jitter u16, Mode, Protocol, STmin, P2min, NAs, NCr (u16), SlaveNAD
        auto p = sys_cmd(sys_send_lin_cfg);
        put8(p, ch);
        put16(p, static_cast<uint16_t>(cfg.lin_baudrate));
        put8(p, cfg.lin_timebase_ms);
        put16(p, cfg.lin_jitter_us);
        put8(p, master ? 0 : (listen_only ? 2 : 1));
        put8(p, static_cast<uint8_t>(cfg.lin_protocol));
        put16(p, t.st_min_ms);
        put16(p, t.p2_min_ms);
        put16(p, t.n_as_ms);
        put16(p, t.n_cr_ms);
        put8(p, lin_db_node_nad(ldb, diag_node));
        send_cmd(dev, p);
        std::this_thread::sleep_for(5ms);

        if (!listen_only)
        {
            int tables = static_cast<int>(ldb.schedule_tables.size());
            if (max_tables > 0 && tables > max_tables)
            {
                log_warning(std::format("GrIP: LDF contains {} schedule tables but device supports only {}, truncating",
                                        tables, max_tables));
                tables = max_tables;
            }
            for (int ti = 0; ti < tables; ++ti)
            {
                lin_set_table(dev, ch, static_cast<uint8_t>(ti));
                std::this_thread::sleep_for(2ms);
                for (const auto& e : ldb.schedule_tables[static_cast<std::size_t>(ti)].entries)
                {
                    if (!master && e.publisher_name != slave)
                    {
                        continue;
                    }
                    const bool rx = master ? e.is_master_publisher : e.publisher_name == slave;
                    const uint8_t dlc = std::min<uint8_t>(e.dlc, 8);
                    // Channel, ID, DLC, Direction (0 = we publish), Delay, Flags, Time u32, Data[8]
                    auto f = sys_cmd(sys_add_lin_frame);
                    put8(f, ch);
                    put8(f, e.frame_id);
                    put8(f, dlc);
                    put8(f, rx ? 0 : 1);
                    put8(f, e.delay_ms);
                    put8(f, e.is_sporadic ? lin_flag_sporadic : 0);
                    put32(f, 0);
                    std::array<uint8_t, 8> data{};
                    if (auto it = cfg.lin_frame_defaults.find(e.frame_id); it != cfg.lin_frame_defaults.end())
                    {
                        std::copy_n(it->second.begin(), std::min<std::size_t>(it->second.size(), dlc), data.begin());
                    }
                    for (uint8_t b : data)
                    {
                        put8(f, b);
                    }
                    send_cmd(dev, f);
                }
            }
            lin_set_table(dev, ch, cfg.lin_schedule_table_index);
            std::this_thread::sleep_for(2ms);
        }
    }
    else
    {
        log_error(std::format("GrIP: cannot load LDF {}: {}", cfg.lin_ldf_path, ldb.last_error));
    }
    std::this_thread::sleep_for(10ms);
    lin_enable(dev, ch, true);
}

// ---------------------------------------------------------------------------
// DriverOps
// ---------------------------------------------------------------------------

void grip_enumerate(std::vector<IfaceInfo>& out)
{
    for (const auto& port : serial_list_ports())
    {
        if (port.vid != grip_vid || port.pid != grip_pid)
        {
            continue;
        }
        // ponytail: first CANIL only (as the Qt driver); channel names are not per device; port->device map when a second CANIL shows up (T52b5).
        grip_port = port.name;
        auto dev = g_device.lock();
        if (!dev || dev->port_name != port.name)
        {
            dev = grip_device_open(port.name);
        }
        if (!dev)
        {
            return;
        }
        GripDeviceInfo info;
        {
            std::lock_guard lock(dev->mutex);
            if (!dev->info_received)
            {
                log_warning(std::format("GrIP {}: no answer to the info request", port.name));
                return;
            }
            info = dev->info;
        }
        auto add = [&](std::string name, bool lin, bool fd, int ch) {
            IfaceInfo i{.name = std::move(name), .version = info.version, .bus_type = lin ? BusType::LIN : BusType::CAN};
            grip_apply_caps(i, lin, fd, grip_request_caps(*dev, lin ? 1 : 0, static_cast<uint8_t>(ch), 50ms));
            out.push_back(std::move(i));
        };
        int n = 0;
        for (int i = 0; i < info.can; ++i, ++n)
        {
            add(std::format("CANIL-CAN{}", n), false, false, n);
        }
        for (int i = 0; i < info.canfd; ++i, ++n)
        {
            add(std::format("CANIL-CANFD{}", n), false, true, n);
        }
        for (int i = 0; i < info.lin; ++i)
        {
            add(std::format("CANIL-LIN{}", i), true, false, i);
        }
        return;
    }
}

bool grip_open(Iface& iface, const IfaceConfig& cfg)
{
    const ChannelId id = parse_channel(iface.info.name);
    auto dev = g_device.lock();
    if (!dev && !grip_port.empty())
    {
        dev = grip_device_open(grip_port);
        g_device = dev;
    }
    if (!dev || id.ch < 0)
    {
        return false;
    }
    const auto ch = static_cast<uint8_t>(id.ch);
    int max_tables = 0;
    {
        std::lock_guard lock(dev->mutex);
        if (!rx_queue(*dev, id.lin, ch))
        {
            log_error(std::format("GrIP {}: device has no channel {}", dev->port_name, iface.info.name));
            return false;
        }
        max_tables = dev->info.lin_tables;
    }
    const uint32_t caps = grip_request_caps(*dev, id.lin ? 1 : 0, ch, 50ms);
    if (id.lin)
    {
        open_lin(*dev, ch, max_tables, cfg);
    }
    else
    {
        open_can(*dev, ch, caps, cfg);
    }
    auto c = std::make_unique<GripChannel>();
    c->dev = std::move(dev);
    c->ch = ch;
    c->lin = id.lin;
    iface_set_impl(iface, std::move(c));
    return true;
}

void grip_close(Iface& iface)
{
    auto& c = iface_impl<GripChannel>(iface);
    if (c.lin)
    {
        lin_enable(*c.dev, c.ch, false);
    }
    else
    {
        can_enable(*c.dev, c.ch, false);
    }
}

bool grip_send(Iface& iface, const BusMessage& msg)
{
    auto& c = iface_impl<GripChannel>(iface);
    auto& dev = *c.dev;
    bool ok = false;
    if (c.lin)
    {
        // Channel, ID, Data[8]
        auto p = sys_cmd(sys_set_lin_data);
        put8(p, c.ch);
        put8(p, static_cast<uint8_t>(msg.id));
        for (int i = 0; i < 8; ++i)
        {
            put8(p, i < msg.len ? msg.data[static_cast<std::size_t>(i)] : 0);
        }
        ok = send_cmd(dev, p);
    }
    else
    {
        const uint8_t len = std::min<uint8_t>(msg.len, 64);
        uint8_t flags = 0;
        flags |= has_flag(msg, bus_flag::extended) ? can_flag_ext : 0;
        flags |= has_flag(msg, bus_flag::fd) ? can_flag_fd : 0;
        flags |= has_flag(msg, bus_flag::brs) ? can_flag_brs : 0;
        flags |= has_flag(msg, bus_flag::rtr) ? can_flag_rtr : 0;

        // FNV-1a over the key fields and the time: correlation token the device echoes back.
        uint32_t hash = 2166136261u;
        auto mix = [&](uint64_t v, int bytes) {
            for (int i = 0; i < bytes; ++i)
            {
                hash = (hash ^ static_cast<uint8_t>(v >> (8 * i))) * 16777619u;
            }
        };
        const auto sent = std::chrono::steady_clock::now();
        mix(c.ch, 1);
        mix(msg.id, 4);
        mix(len, 1);
        mix(flags, 1);
        mix(static_cast<uint64_t>(sent.time_since_epoch().count()), 8);

        // Channel, ID u32, DLC, Flags, ErrFlags, Time u32 (= hash), Data[len]
        auto p = sys_cmd(sys_send_can_frame);
        put8(p, c.ch);
        put32(p, msg.id);
        put8(p, len);
        put8(p, flags);
        put8(p, 0);
        put32(p, hash);
        for (int i = 0; i < len; ++i)
        {
            put8(p, msg.data[static_cast<std::size_t>(i)]);
        }
        {
            BusMessage echo = msg;
            echo.flags |= bus_flag::tx;
            echo.iface = iface.index;
            echo.ts_ns = now_ns();
            std::lock_guard lock(dev.mutex);
            dev.tx_pending.insert_or_assign(hash, GripTxPending{.ch = c.ch, .msg = echo, .sent = sent});
        }
        ok = send_cmd(dev, p);
    }
    if (!ok)
    {
        c.tx_errors.fetch_add(1, std::memory_order_relaxed);
    }
    return ok;
}

int grip_read(Iface& iface, BusMessage* out, int max, int timeout_ms)
{
    auto& c = iface_impl<GripChannel>(iface);
    auto& dev = *c.dev;
    std::unique_lock lock(dev.mutex);
    dev.cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
        const auto* q = rx_queue(dev, c.lin, c.ch);
        return dev.failed || (q && !q->empty());
    });
    if (dev.failed)
    {
        return -1;
    }
    auto* q = rx_queue(dev, c.lin, c.ch);
    int n = 0;
    while (q && !q->empty() && n < max)
    {
        BusMessage& m = out[n++];
        m = q->front();
        q->pop_front();
        m.iface = iface.index;
        if (!has_flag(m, bus_flag::tx))
        {
            c.rx_frames.fetch_add(1, std::memory_order_relaxed);
        }
        else if (is_error_frame(m) && m.type == BusType::CAN)
        {
            c.tx_errors.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            c.tx_frames.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return n;
}

void grip_stats(Iface& iface, IfaceStats& out)
{
    auto& c = iface_impl<GripChannel>(iface);
    auto& dev = *c.dev;
    uint8_t state = grip_can_off;
    {
        std::lock_guard lock(dev.mutex);
        const auto& states = c.lin ? dev.lin_state : dev.can_state;
        if (c.ch < states.size())
        {
            state = states[c.ch];
        }
        if (!c.lin && c.ch < dev.can_rx_drops.size())
        {
            out.rx_overruns = dev.can_rx_drops[c.ch];
        }
    }
    switch (state)
    {
    case grip_can_active:
        out.state = IfaceState::Ok;
        break;
    case grip_can_error_warning:
        out.state = IfaceState::Warning;
        break;
    case grip_can_error_passive:
        out.state = IfaceState::Passive;
        break;
    case grip_can_off:
        out.state = IfaceState::BusOff;
        break;
    case grip_can_stopped:
        out.state = IfaceState::Stopped;
        break;
    default:
        out.state = IfaceState::Unknown;
        break;
    }
    out.rx_frames = c.rx_frames;
    out.tx_frames = c.tx_frames;
    out.tx_errors = c.tx_errors;
}

void grip_lin_sleep_wakeup(Iface& iface, bool wakeup)
{
    auto& c = iface_impl<GripChannel>(iface);
    if (c.lin)
    {
        auto p = sys_cmd(sys_lin_sleep_wakeup);
        put8(p, c.ch);
        put8(p, wakeup ? 1 : 0);
        send_cmd(*c.dev, p);
    }
}

void grip_lin_set_schedule(Iface& iface, uint8_t table)
{
    auto& c = iface_impl<GripChannel>(iface);
    if (c.lin)
    {
        lin_set_table(*c.dev, c.ch, table);
    }
}

void grip_lin_diag_request(Iface& iface, uint8_t nad, std::span<const uint8_t> data)
{
    auto& c = iface_impl<GripChannel>(iface);
    if (!c.lin || data.empty() || data.size() > 250)
    {
        return;
    }
    // Channel, NAD, SID + request bytes; the firmware adds the PCI
    auto p = sys_cmd(sys_lin_diag_req);
    put8(p, c.ch);
    put8(p, nad);
    for (uint8_t b : data)
    {
        put8(p, b);
    }
    send_cmd(*c.dev, p);
}

} // namespace

// ---------------------------------------------------------------------------
// Codec
// ---------------------------------------------------------------------------

bool grip_encode(std::string& out, uint8_t msg_type, uint8_t ret, std::span<const uint8_t> payload)
{
    if (payload.size() > grip::max_payload)
    {
        return false;
    }
    const auto len = static_cast<uint16_t>(payload.size());
    std::array<uint8_t, 8> hdr = {grip::version, 0, msg_type, ret, static_cast<uint8_t>(len),
                                  static_cast<uint8_t>(len >> 8), 0, 0};
    hdr[7] = len > 0 ? grip::crc8(payload) : 0;
    hdr[6] = grip::crc8(std::span(hdr).first(6));
    out += static_cast<char>(soh);
    append_hex(out, hdr);
    if (len > 0)
    {
        out += static_cast<char>(sot);
        append_hex(out, payload);
    }
    out += static_cast<char>(eot);
    return true;
}

void grip_parse(GripParser& p, std::span<const uint8_t> bytes)
{
    using State = GripParser::State;
    for (uint8_t c : bytes)
    {
        switch (p.state)
        {
        case State::Idle:
            if (c == soh)
            {
                p.state = State::Header;
                p.count = 0;
            }
            break; // anything else (EOT, noise) is dropped

        case State::Header:
        {
            p.hex[p.count++] = static_cast<char>(c);
            if (p.count < 16)
            {
                break;
            }
            std::array<uint8_t, 8> h{};
            for (std::size_t i = 0; i < 8; ++i)
            {
                h[i] = static_cast<uint8_t>(std::max(slcan::from_hex_nibble(p.hex[2 * i]), 0) << 4 |
                                            std::max(slcan::from_hex_nibble(p.hex[2 * i + 1]), 0));
            }
            p.state = State::Idle;
            if (h[0] != grip::version || h[2] >= grip::msg_max)
            {
                break;
            }
            if (h[6] != grip::crc8(std::span(h).first(6)))
            {
                ++p.crc_errors;
                break;
            }
            p.pkt = GripPacket{.msg_type = h[2], .length = get16(h, 4)};
            p.crc_data = h[7];
            if (p.pkt.length > grip::max_payload)
            {
                break;
            }
            if (p.pkt.msg_type == grip::msg_response || p.pkt.msg_type == grip::msg_sync ||
                p.pkt.msg_type == grip::msg_error)
            {
                break; // acknowledgements of our own packets carry nothing we use
            }
            if (p.pkt.length == 0)
            {
                p.packets.push_back(p.pkt);
                break;
            }
            p.state = State::WaitSot;
            break;
        }

        case State::WaitSot:
            p.state = c == sot ? State::Data : (c == soh ? State::Header : State::Idle);
            p.count = 0;
            break;

        case State::Data:
        {
            p.hex[p.count % 2] = static_cast<char>(c);
            if (++p.count % 2 != 0)
            {
                break;
            }
            const int hi = slcan::from_hex_nibble(p.hex[0]);
            const int lo = slcan::from_hex_nibble(p.hex[1]);
            if (hi < 0 || lo < 0)
            {
                p.acks.push_back(grip::ret_wrong_param);
                p.state = State::Idle;
                break;
            }
            const std::size_t i = p.count / 2 - 1;
            p.pkt.data[i] = static_cast<uint8_t>(hi << 4 | lo);
            if (i + 1 < p.pkt.length)
            {
                break;
            }
            p.state = State::Idle;
            if (grip::crc8(std::span(p.pkt.data).first(p.pkt.length)) != p.crc_data)
            {
                ++p.crc_errors;
                break;
            }
            p.packets.push_back(p.pkt);
            if (p.pkt.msg_type != grip::msg_data_no_response)
            {
                p.acks.push_back(grip::ret_ok);
            }
            break;
        }
        }
    }
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

std::shared_ptr<GripDevice> grip_device_open(const std::string& port_name)
{
    std::shared_ptr<GripDevice> dev(new GripDevice, [](GripDevice* d) {
        d->worker = {};
        serial_close(d->port);
        delete d;
    });
    dev->port_name = port_name;
    if (!serial_open(dev->port, port_name, grip_baud))
    {
        log_error(std::format("GrIP: {}", dev->port.error));
        return nullptr;
    }
    // Stale bytes of a previous session would only produce framing noise.
    serial_clear(dev->port);
    dev->worker = std::jthread(worker, std::ref(*dev), std::weak_ptr<GripDevice>(dev));

    send_cmd(*dev, sys_cmd(sys_report_info));
    std::unique_lock lock(dev->mutex);
    dev->cv.wait_for(lock, 200ms, [&] { return dev->info_received || dev->failed; });
    return dev;
}

uint32_t grip_request_caps(GripDevice& dev, uint8_t bus_type, uint8_t ch, std::chrono::milliseconds timeout)
{
    const auto key = static_cast<uint16_t>(bus_type << 8 | ch);
    uint32_t known = 0;
    {
        std::lock_guard lock(dev.mutex);
        if (auto it = dev.caps.find(key); it != dev.caps.end())
        {
            known = it->second;
            dev.caps.erase(it); // wait for a fresh answer below
        }
    }
    auto p = sys_cmd(sys_get_channel_caps);
    put8(p, bus_type);
    put8(p, ch);
    put32(p, 0);
    send_cmd(dev, p);

    std::unique_lock lock(dev.mutex);
    dev.cv.wait_for(lock, timeout, [&] { return dev.failed || dev.caps.contains(key); });
    if (auto it = dev.caps.find(key); it != dev.caps.end())
    {
        return it->second;
    }
    dev.caps[key] = known;   // no answer: keep the previous bits
    return known;
}

void grip_attach(Tasks& tasks, std::deque<Iface>& ifaces)
{
    g_tasks = &tasks;
    g_ifaces = &ifaces;
}

std::shared_ptr<GripDevice> grip_open_device()
{
    return g_device.lock();
}

void grip_gpio_config(GripDevice& dev, bool enable, uint8_t cycle_ms, uint16_t dir_mask)
{
    auto p = sys_cmd(sys_send_gpio_cfg, enable ? 1 : 0);
    put8(p, cycle_ms);
    put16(p, dir_mask);
    send_cmd(dev, p);
}

void grip_gpio_output(GripDevice& dev, uint16_t mask)
{
    auto p = sys_cmd(sys_set_gpio_output);
    put16(p, mask);
    send_cmd(dev, p);
}

void grip_apply_caps(IfaceInfo& info, bool lin, bool fd_channel, uint32_t caps)
{
    info.bitrates.clear();
    if (lin)
    {
        info.details = "CANIL with LIN support";
        info.capabilities = 0;
        info.capabilities |= (caps & grip_cap::lin_mode_master) ? iface_cap::lin_master : 0;
        info.capabilities |= (caps & grip_cap::lin_mode_slave) ? iface_cap::lin_slave : 0;
        return;
    }
    const bool fd = caps != 0 ? (caps & grip_cap::can_fd) != 0 : fd_channel;
    info.details = (caps & grip_cap::can_fd) ? "CANIL with CANFD support" : "CANIL with standard CAN support";
    if (caps == 0)
    {
        info.capabilities = iface_cap::auto_restart | iface_cap::listen_only | (fd ? iface_cap::canfd : 0);
    }
    else
    {
        info.capabilities = 0;
        info.capabilities |= (caps & grip_cap::can_listen_only) ? iface_cap::listen_only : 0;
        info.capabilities |= (caps & grip_cap::can_abom) ? iface_cap::auto_restart : 0;
        info.capabilities |= fd ? iface_cap::canfd : 0;
    }

    struct Rate
    {
        unsigned rate;
        uint32_t cap;   // 0: no firmware bit, only offered while the caps are unknown
    };
    static constexpr Rate rates[] = {
        {10000, grip_cap::can_baud_10k},   {20000, grip_cap::can_baud_20k},   {50000, grip_cap::can_baud_50k},
        {100000, grip_cap::can_baud_100k}, {125000, grip_cap::can_baud_125k}, {250000, grip_cap::can_baud_250k},
        {500000, grip_cap::can_baud_500k}, {800000, 0},                       {1000000, grip_cap::can_baud_1m},
    };
    for (const auto& r : rates)
    {
        if (caps != 0 && (r.cap == 0 || !(caps & r.cap)))
        {
            continue;
        }
        if (!fd)
        {
            info.bitrates.push_back({.bitrate = r.rate});
            continue;
        }
        for (unsigned data : {2000000u, 5000000u})
        {
            info.bitrates.push_back({.bitrate = r.rate, .bitrate_fd = data, .sample_point = 875, .sample_point_fd = 750});
        }
    }
}

extern const DriverOps grip_driver = {
    .name = "GrIP-CANIL",
    .enumerate = grip_enumerate,
    .open = grip_open,
    .close = grip_close,
    .send = grip_send,
    .read = grip_read,
    .stats = grip_stats,
    .lin_sleep_wakeup = grip_lin_sleep_wakeup,
    .lin_set_schedule = grip_lin_set_schedule,
    .lin_diag_request = grip_lin_diag_request,
};
