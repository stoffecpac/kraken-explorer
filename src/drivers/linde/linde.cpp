/*

  Copyright (c) 2026 Schildkroet

  This file is part of Kraken Explorer.

  Kraken Explorer is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  Kraken Explorer is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Kraken Explorer.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "drivers/linde/linde.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/log.h"
#include "drivers/driver.h"
#include "drivers/usb_vendor/usb_vendor.h"

namespace
{

constexpr int max_channels = 4;
constexpr std::size_t max_queued_frames = 4096; // per channel, oldest dropped (overrun)
constexpr uint8_t max_tables = 8;
constexpr unsigned ctrl_timeout_ms = 1000;
constexpr unsigned bulk_timeout_ms = 50;

constexpr UsbVendorId usb_id{
    .vid = LIN_USB_VID,
    .pid = LIN_USB_PID,
    .protocol = LIN_USB_ITF_PROTOCOL,
};

// One physical lin_usb device. All channels share one bulk IN endpoint, so one reader
// thread demultiplexes into per-channel queues that each channel's read() waits on.
struct LindeDevice
{
    int index = 0; // n-th device exposing the LIN interface
    UsbVendor usb;

    // From DEVICE_CONFIG; only change in device_open/_close while the reader is stopped.
    uint8_t channel_count = 0; // clamped to max_channels
    uint8_t schedule_tables = 0;
    uint8_t schedule_entries = 0;
    uint32_t features = 0; // LIN_USB_FEATURE_*
    uint32_t sw_version = 0;
    uint32_t hw_version = 0;

    // Reference-counted open/close; held across a channel's whole open/close sequence so
    // two channels never interleave their EP0 configuration bursts.
    std::mutex open_mutex;
    int open_count = 0;

    std::shared_mutex handle_lock; // transfers shared, device_close unique
    std::mutex write_mutex;        // one shared bulk OUT endpoint
    std::mutex control_mutex;      // EP0

    std::mutex queue_mutex; // guards the three arrays below
    std::condition_variable queue_cond;
    std::array<std::deque<lin_usb_host_frame_t>, max_channels> rx_queues;
    std::array<bool, max_channels> channel_open{};
    std::array<uint64_t, max_channels> rx_overruns{};

    // Written on first open, before any listener of this device runs; read by listeners.
    uint32_t device_start_ms = 0;
    int64_t host_start_ns = 0;
    bool timestamp_valid = false;

    std::jthread reader;
};

// Per-channel state in Iface::impl.
struct LindeChannel
{
    std::shared_ptr<LindeDevice> dev;
    uint8_t ch = 0;
    std::atomic<uint64_t> rx{0};
    std::atomic<uint64_t> tx{0};
    std::atomic<uint64_t> rx_errors{0};
    std::atomic<uint64_t> tx_errors{0};
    std::atomic<uint32_t> device_dropped{0}; // refreshed by stats()
    std::array<uint8_t, max_tables> table_entry_counts{};
};

// Rebuilt by enumerate (main thread, no measurement running); open() looks devices up here.
std::vector<std::shared_ptr<LindeDevice>> devices;

template <class T>
bool control_out(LindeDevice& d, uint8_t breq, uint16_t value, const T& data)
{
    std::shared_lock handle(d.handle_lock);
    std::lock_guard lock(d.control_mutex);
    return usb_vendor_control_out(d.usb, breq, value, &data, sizeof(T), ctrl_timeout_ms) == UsbStatus::Ok;
}

bool control_in(LindeDevice& d, uint8_t breq, uint16_t value, void* data, uint16_t len)
{
    std::shared_lock handle(d.handle_lock);
    std::lock_guard lock(d.control_mutex);
    return usb_vendor_control_in(d.usb, breq, value, data, len, ctrl_timeout_ms) == UsbStatus::Ok;
}

bool send_frame(LindeDevice& d, const lin_usb_host_frame_t& frame)
{
    std::shared_lock handle(d.handle_lock);
    std::lock_guard lock(d.write_mutex);
    return usb_vendor_bulk_write(d.usb, &frame, sizeof(frame), ctrl_timeout_ms) == UsbStatus::Ok;
}

bool set_mode(LindeDevice& d, uint8_t ch, uint8_t mode)
{
    return control_out(d, LIN_USB_BREQ_MODE, ch, lin_usb_mode_t{.mode = mode});
}

bool schedule_start(LindeDevice& d, uint8_t ch, uint8_t table, uint8_t entry_count)
{
    return control_out(d, LIN_USB_BREQ_MODE, ch,
                       lin_usb_mode_t{.mode = LIN_USB_MODE_START, .table_id = table, .entry_count = entry_count});
}

bool upload_schedule_entry(LindeDevice& d, uint8_t ch, uint8_t table, uint8_t slot, lin_usb_schedule_entry_t entry)
{
    entry.table_id = table;
    // wValue: slot index in the high byte, channel in the low byte
    return control_out(d, LIN_USB_BREQ_SCHEDULE, static_cast<uint16_t>((slot << 8) | ch), entry);
}

bool device_open(LindeDevice& d)
{
    if (!usb_vendor_open(d.usb, usb_id, d.index))
    {
        return false;
    }
    lin_usb_device_config_t cfg{};
    if (!control_in(d, LIN_USB_BREQ_DEVICE_CONFIG, 0, &cfg, sizeof(cfg)))
    {
        usb_vendor_close(d.usb);
        return false;
    }
    // Unsigned so icount 0xFF cannot wrap to zero channels.
    const unsigned reported = cfg.icount + 1u;
    if (reported > max_channels)
    {
        log_warning(std::format("LindeAPI: device reports {} channels, only the first {} are supported", reported,
                                max_channels));
    }
    d.channel_count = static_cast<uint8_t>(std::min<unsigned>(reported, max_channels));
    d.schedule_tables = cfg.schedule_tables;
    // Older firmware leaves the field zero; fall back to the historic value.
    d.schedule_entries = cfg.schedule_entries ? cfg.schedule_entries : uint8_t{LIN_USB_MAX_SCHEDULE_ENTRIES};
    d.features = cfg.features;
    d.sw_version = cfg.sw_version;
    d.hw_version = cfg.hw_version;

    std::lock_guard lock(d.queue_mutex);
    for (auto& q : d.rx_queues)
    {
        q.clear();
    }
    d.channel_open = {};
    d.rx_overruns = {};
    return true;
}

void device_close(LindeDevice& d)
{
    std::unique_lock lock(d.handle_lock);
    usb_vendor_close(d.usb);
    d.channel_count = 0;
}

void reader_loop(std::stop_token stop, LindeDevice& d)
{
    bool error_logged = false;
    while (!stop.stop_requested())
    {
        lin_usb_host_frame_t frame{};
        int transferred = 0;
        const UsbStatus status = usb_vendor_bulk_read(d.usb, &frame, sizeof(frame), transferred, bulk_timeout_ms);
        if (status == UsbStatus::Timeout)
        {
            continue;
        }
        if (status != UsbStatus::Ok)
        {
            // An unplugged device fails immediately: log once and back off.
            if (!error_logged)
            {
                log_error(std::format("LindeAPI: USB read failed: {}", usb_vendor_last_error(d.usb)));
                error_logged = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(bulk_timeout_ms));
            continue;
        }
        error_logged = false;
        if (transferred != static_cast<int>(sizeof(frame)))
        {
            continue;
        }
        const uint8_t ch = frame.channel;
        if (ch >= d.channel_count)
        {
            log_error(std::format("LindeAPI: received frame for invalid channel {}", ch));
            continue;
        }
        std::lock_guard lock(d.queue_mutex);
        // Frames for a closed channel would be delivered stale on its next open.
        if (!d.channel_open[ch])
        {
            continue;
        }
        auto& q = d.rx_queues[ch];
        if (q.size() >= max_queued_frames)
        {
            q.pop_front();
            ++d.rx_overruns[ch];
        }
        q.push_back(frame);
        d.queue_cond.notify_all();
    }
}

void reader_stop(LindeDevice& d)
{
    d.reader = {};
    std::lock_guard lock(d.queue_mutex);
    for (auto& q : d.rx_queues)
    {
        q.clear();
    }
}

void set_channel_open(LindeDevice& d, uint8_t ch, bool open)
{
    std::lock_guard lock(d.queue_mutex);
    d.channel_open[ch] = open;
    d.rx_queues[ch].clear();
    if (open)
    {
        d.rx_overruns[ch] = 0;
    }
    d.queue_cond.notify_all();
}

void reset_timestamp_epoch(LindeDevice& d)
{
    uint32_t ts = 0;
    // The device clock is shared by all channels; channel 0 always exists.
    const bool ok = control_in(d, LIN_USB_BREQ_TIMESTAMP, 0, &ts, sizeof(ts));
    d.device_start_ms = ts;
    d.host_start_ns = now_ns();
    d.timestamp_valid = ok && (d.features & LIN_USB_FEATURE_TIMESTAMP);
}

// Undoes a channel's claim on its device; caller holds open_mutex.
void release_device(LindeDevice& d, uint8_t ch)
{
    set_channel_open(d, ch, false);
    if (--d.open_count == 0)
    {
        reader_stop(d);
        device_close(d);
    }
}

std::string iface_name(int device, int ch)
{
    return std::format("Linde{}_CH{}", device, ch);
}

void linde_enumerate(std::vector<IfaceInfo>& out)
{
    devices.clear();
    // 1d50:606f is also gs_usb, so only devices that expose the LIN interface count.
    const int count = usb_vendor_count(usb_id);
    for (int i = 0; i < count; ++i)
    {
        auto dev = std::make_shared<LindeDevice>();
        dev->index = i;
        // Brief open to read the channel count; channels reopen on measurement start.
        if (!device_open(*dev))
        {
            log_warning(std::format("LindeAPI: cannot open device {}: {}", i, usb_vendor_last_error(dev->usb)));
            continue;
        }
        const uint32_t sw = dev->sw_version; // major << 16 | minor << 8 | patch
        const std::string version =
            std::format("{}.{}.{} (HW {})", (sw >> 16) & 0xFFu, (sw >> 8) & 0xFFu, sw & 0xFFu, dev->hw_version);
        const uint8_t channels = dev->channel_count;
        device_close(*dev);
        for (uint8_t ch = 0; ch < channels; ++ch)
        {
            IfaceInfo info{
                .name = iface_name(i, ch),
                .details = std::format("LIN Interface CH{}", ch),
                .version = version,
                .bus_type = BusType::LIN,
                .capabilities = iface_cap::lin_master | iface_cap::lin_slave,
            };
            for (unsigned rate : {1200u, 2400u, 4800u, 9600u, 10417u, 19200u, 20000u})
            {
                info.bitrates.push_back({.bitrate = rate, .sample_point = 0});
            }
            out.push_back(std::move(info));
        }
        devices.push_back(std::move(dev));
    }
}

// Payload the device transmits for a publisher slot, from the configured frame defaults.
void fill_default_payload(const IfaceConfig& config, lin_usb_schedule_entry_t& se)
{
    if (auto it = config.lin_frame_defaults.find(se.lin_id); it != config.lin_frame_defaults.end())
    {
        std::copy_n(it->second.begin(), std::min<std::size_t>({it->second.size(), se.dlc, sizeof(se.data)}), se.data);
    }
}

// Master: uploads the LDF schedule tables and starts the selected one.
void upload_master_schedule(LindeChannel& c, const IfaceConfig& config, const LinDb& ldb)
{
    LindeDevice& d = *c.dev;
    const int device_tables = d.schedule_tables > 0 ? d.schedule_tables : max_tables;
    const int ldf_tables = static_cast<int>(ldb.schedule_tables.size());
    const int table_count = std::min({ldf_tables, device_tables, static_cast<int>(max_tables)});
    if (ldf_tables > table_count)
    {
        log_warning(std::format("LindeAPI: LDF has {} schedule tables, device supports {}; extra tables ignored",
                                ldf_tables, table_count));
    }
    for (int t = 0; t < table_count; ++t)
    {
        const auto& entries = ldb.schedule_tables[static_cast<std::size_t>(t)].entries;
        const int entry_count = std::min(static_cast<int>(entries.size()), static_cast<int>(d.schedule_entries));
        if (static_cast<int>(entries.size()) > entry_count)
        {
            log_warning(std::format("LindeAPI: schedule table {} has {} entries, device supports {}; extra entries ignored",
                                    t, entries.size(), d.schedule_entries));
        }
        for (int s = 0; s < entry_count; ++s)
        {
            const LinScheduleEntry& le = entries[static_cast<std::size_t>(s)];
            lin_usb_schedule_entry_t se{
                .lin_id = le.frame_id,
                // 0 = device is publisher (TX), 1 = subscriber (RX)
                .direction = le.is_master_publisher ? uint8_t{0} : uint8_t{1},
                .dlc = le.dlc,
                .flags = le.is_sporadic ? uint8_t{LIN_USB_FRAME_FLAG_SPORADIC} : uint8_t{0},
                .period_ms = le.delay_ms,
            };
            if (le.is_master_publisher)
            {
                fill_default_payload(config, se);
            }
            if (!upload_schedule_entry(d, c.ch, static_cast<uint8_t>(t), static_cast<uint8_t>(s), se))
            {
                log_warning(std::format("LindeAPI: uploadScheduleEntry failed for table {} slot {}", t, s));
            }
        }
        c.table_entry_counts[static_cast<std::size_t>(t)] = static_cast<uint8_t>(entry_count);
    }
    if (table_count == 0)
    {
        log_warning("LindeAPI: LDF has no schedule tables, master schedule not started");
        return;
    }
    const auto active = static_cast<uint8_t>(std::clamp(static_cast<int>(config.lin_schedule_table_index), 0, table_count - 1));
    if (!schedule_start(d, c.ch, active, c.table_entry_counts[active]))
    {
        log_warning(std::format("LindeAPI: scheduleStart failed: {}", usb_vendor_last_error(d.usb)));
    }
}

// Slave: the firmware only reacts to PIDs in the active table. Frames this node publishes
// go in as publisher entries (it answers them), every other LDF frame as a subscriber
// entry so it is reported. Deduplicated by id; if slots run out, publishing wins.
void upload_slave_schedule(LindeChannel& c, const IfaceConfig& config, const LinDb* ldb)
{
    constexpr uint8_t slave_table = 0;
    LindeDevice& d = *c.dev;
    const unsigned max_slots = d.schedule_entries;
    uint8_t slot = 0;
    if (ldb)
    {
        std::set<uint8_t> uploaded;
        int dropped = 0;
        for (const bool publisher_pass : {true, false})
        {
            for (const auto& table : ldb->schedule_tables)
            {
                for (const LinScheduleEntry& le : table.entries)
                {
                    if ((le.publisher_name == config.lin_slave_node) != publisher_pass || uploaded.contains(le.frame_id))
                    {
                        continue;
                    }
                    if (slot >= max_slots)
                    {
                        ++dropped;
                        continue;
                    }
                    uploaded.insert(le.frame_id);
                    lin_usb_schedule_entry_t se{
                        .lin_id = le.frame_id,
                        // 0 = this slave answers the header, 1 = another node does
                        .direction = publisher_pass ? uint8_t{0} : uint8_t{1},
                        .dlc = le.dlc,
                    };
                    if (publisher_pass)
                    {
                        fill_default_payload(config, se);
                    }
                    if (!upload_schedule_entry(d, c.ch, slave_table, slot, se))
                    {
                        log_warning(std::format("LindeAPI: uploadScheduleEntry failed for slave slot {}", slot));
                    }
                    ++slot;
                }
            }
        }
        if (dropped > 0)
        {
            log_warning(std::format(
                "LindeAPI: LDF has more frames than the device's {} slave slots, {} frame(s) not observable", max_slots,
                dropped));
        }
    }
    // Start even with no frames: the slave still reports diagnostic frames.
    c.table_entry_counts[slave_table] = slot;
    schedule_start(d, c.ch, slave_table, slot);
}

// Channel configuration after the device is open; false = release the claim.
bool configure_channel(LindeChannel& c, const IfaceConfig& config)
{
    LindeDevice& d = *c.dev;
    set_channel_open(d, c.ch, true);
    control_out(d, LIN_USB_BREQ_HOST_FORMAT, c.ch, lin_usb_host_config_t{.byte_order = 0xEF});

    const bool master = config.lin_node_mode == LinNodeMode::Master;
    const bool listen_only = config.lin_listen_only || config.lin_node_mode == LinNodeMode::Monitor;

    constexpr uint8_t protocol_map[] = {LIN_USB_VERSION_1_3, LIN_USB_VERSION_2_0, LIN_USB_VERSION_2_1,
                                        LIN_USB_VERSION_2_2, LIN_USB_VERSION_2_2A};
    const auto protocol = static_cast<std::size_t>(config.lin_protocol);
    lin_usb_bus_config_t bus{
        .baudrate = config.lin_baudrate,
        .lin_version = protocol < std::size(protocol_map) ? protocol_map[protocol] : uint8_t{LIN_USB_VERSION_2_1},
        .break_length = 13, // standard LIN break
        .timebase_ms = config.lin_timebase_ms,
        .jitter_us = config.lin_jitter_us,
        .flags = (master && !listen_only) ? uint8_t{LIN_USB_FLAG_MASTER} : uint8_t{0},
    };
    if (listen_only)
    {
        bus.flags |= LIN_USB_FLAG_LISTEN_ONLY;
        if (!(d.features & LIN_USB_FEATURE_LISTEN_ONLY))
        {
            log_warning("LindeAPI: device does not support listen-only mode, it will act as a silent slave instead");
        }
    }

    std::unique_ptr<LinDb> ldb;
    if (!config.lin_ldf_path.empty())
    {
        ldb = std::make_unique<LinDb>();
        if (!lin_db_load(*ldb, config.lin_ldf_path))
        {
            ldb.reset();
        }
    }
    if (ldb)
    {
        const std::string diag_node = lin_db_diag_node(*ldb, master, config.lin_slave_node);
        const LinDiagTiming timing = lin_db_diag_timing(*ldb, diag_node);
        bus.slave_nad = lin_db_node_nad(*ldb, diag_node);
        bus.diag_stmin_ms = timing.st_min_ms;
        bus.diag_p2min_ms = timing.p2_min_ms;
        bus.diag_nas_ms = timing.n_as_ms;
        bus.diag_ncr_ms = timing.n_cr_ms;
    }

    // Stop first: the channel may still run from an earlier session, and the bus config
    // reprograms the UART and drops the previous schedule.
    if (!set_mode(d, c.ch, LIN_USB_MODE_STOP))
    {
        log_error(std::format("LindeAPI: setMode failed: {}", usb_vendor_last_error(d.usb)));
        return false;
    }
    if (!control_out(d, LIN_USB_BREQ_BAUDRATE, c.ch, bus))
    {
        log_error(std::format("LindeAPI: setBusConfig failed: {}", usb_vendor_last_error(d.usb)));
        return false;
    }

    c.table_entry_counts = {};
    if (listen_only)
    {
        // Monitor: no schedule; entry_count 0 leaves the device's tables untouched.
        if (!schedule_start(d, c.ch, 0, 0))
        {
            log_warning(std::format("LindeAPI: start (listen-only) failed: {}", usb_vendor_last_error(d.usb)));
        }
    }
    else if (master && ldb)
    {
        upload_master_schedule(c, config, *ldb);
    }
    else if (!master)
    {
        upload_slave_schedule(c, config, ldb.get());
    }
    return true;
}

bool linde_open(Iface& iface, const IfaceConfig& config)
{
    int dev_index = -1;
    int ch = -1;
    if (std::sscanf(iface.info.name.c_str(), "Linde%d_CH%d", &dev_index, &ch) != 2 || ch < 0 || ch >= max_channels)
    {
        return false;
    }
    const auto it = std::ranges::find_if(devices, [&](const auto& d) { return d->index == dev_index; });
    if (it == devices.end())
    {
        log_error(std::format("LindeAPI: device {} not found, rescan interfaces", dev_index));
        return false;
    }
    auto c = std::make_unique<LindeChannel>();
    c->dev = *it;
    c->ch = static_cast<uint8_t>(ch);
    LindeDevice& d = *c->dev;

    std::lock_guard lock(d.open_mutex);
    if (d.open_count == 0)
    {
        if (!device_open(d))
        {
            log_error(std::format("LindeAPI: open failed: {}", usb_vendor_last_error(d.usb)));
            return false;
        }
        d.reader = std::jthread(reader_loop, std::ref(d));
        reset_timestamp_epoch(d);
    }
    ++d.open_count;
    if (c->ch >= d.channel_count)
    {
        log_error(std::format("LindeAPI: channel {} not available (device has {})", ch, d.channel_count));
        release_device(d, c->ch);
        return false;
    }
    if (!configure_channel(*c, config))
    {
        release_device(d, c->ch);
        return false;
    }
    iface_set_impl(iface, std::move(c));
    return true;
}

void linde_close(Iface& iface)
{
    auto& c = iface_impl<LindeChannel>(iface);
    LindeDevice& d = *c.dev;
    std::lock_guard lock(d.open_mutex);
    set_mode(d, c.ch, LIN_USB_MODE_STOP);
    release_device(d, c.ch);
}

// LIN has no immediate transmit: this sets the payload the matching publisher slot sends on
// its next schedule event. The device acknowledges it (see read()); TX is counted when the
// frame actually goes out.
bool linde_send(Iface& iface, const BusMessage& msg)
{
    auto& c = iface_impl<LindeChannel>(iface);
    lin_usb_host_frame_t frame{
        .lin_id = static_cast<uint8_t>(msg.id),
        .channel = c.ch,
        .dlc = static_cast<uint8_t>(std::min<int>(msg.len, 8)),
        .flags = has_flag(msg, bus_flag::lin_sleep) ? uint8_t{LIN_USB_FRAME_FLAG_SLEEP} : uint8_t{0},
    };
    std::memcpy(frame.data, msg.data.data(), frame.dlc);
    if (!send_frame(*c.dev, frame))
    {
        ++c.tx_errors;
        log_error(std::format("LindeAPI: set frame data failed: {}", usb_vendor_last_error(c.dev->usb)));
        return false;
    }
    return true;
}

int linde_read(Iface& iface, BusMessage* out, int max, int timeout_ms)
{
    auto& c = iface_impl<LindeChannel>(iface);
    LindeDevice& d = *c.dev;
    std::array<lin_usb_host_frame_t, 64> frames;
    std::size_t count = 0;
    {
        std::unique_lock lock(d.queue_mutex);
        auto& q = d.rx_queues[c.ch];
        if (!d.queue_cond.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return !q.empty(); }))
        {
            return 0;
        }
        count = std::min({q.size(), frames.size(), static_cast<std::size_t>(max)});
        std::copy_n(q.begin(), count, frames.begin());
        q.erase(q.begin(), q.begin() + static_cast<std::ptrdiff_t>(count));
    }

    int n = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        const lin_usb_host_frame_t& f = frames[i];
        if (f.echo_id == LIN_USB_ECHO_ID_SET_DATA_ACK)
        {
            // Acknowledgement of a send(), not a bus event.
            if (f.flags & LIN_USB_FRAME_FLAG_ERROR)
            {
                ++c.tx_errors;
                log_warning(std::format(
                    "LindeAPI: LIN ID 0x{:02x} is not a publisher frame in the running schedule, data not updated",
                    f.lin_id));
            }
            continue;
        }
        BusMessage& m = out[n++];
        linde_frame_to_message(f, m);
        m.iface = iface.index;
        m.ts_ns = (f.timestamp_ms != 0 && d.timestamp_valid)
                      ? d.host_start_ns + (static_cast<int64_t>(f.timestamp_ms) - d.device_start_ms) * 1'000'000
                      : now_ns();
        ++(has_flag(m, bus_flag::tx) ? c.tx : c.rx);
        if (is_error_frame(m))
        {
            ++c.rx_errors;
        }
    }
    return n;
}

void linde_stats(Iface& iface, IfaceStats& out)
{
    auto& c = iface_impl<LindeChannel>(iface);
    LindeDevice& d = *c.dev;
    out.rx_frames = c.rx;
    out.tx_frames = c.tx;
    out.rx_errors = c.rx_errors;
    out.tx_errors = c.tx_errors;
    out.state = IfaceState::Ok;
    if (d.features & LIN_USB_FEATURE_BUS_STATE)
    {
        lin_usb_bus_state_t hw{};
        if (!control_in(d, LIN_USB_BREQ_BUS_STATE, c.ch, &hw, sizeof(hw)))
        {
            out.state = IfaceState::Unknown;
        }
        else
        {
            c.device_dropped = hw.dropped;
            switch (hw.state)
            {
            case LIN_USB_BUS_STATE_OK:
                out.state = IfaceState::Ok;
                break;
            case LIN_USB_BUS_STATE_PASSIVE:
                out.state = IfaceState::Passive;
                break;
            case LIN_USB_BUS_STATE_BUS_OFF:
                out.state = IfaceState::BusOff;
                break;
            case LIN_USB_BUS_STATE_ERROR:
                out.state = IfaceState::Warning;
                break;
            case LIN_USB_BUS_STATE_STOPPED:
            case LIN_USB_BUS_STATE_SLEEPING: // open, bus idle until the next wakeup
                out.state = IfaceState::Stopped;
                break;
            default:
                out.state = IfaceState::Unknown;
                break;
            }
        }
    }
    std::lock_guard lock(d.queue_mutex);
    // Host queue drops plus frames the device itself could not queue.
    out.rx_overruns = d.rx_overruns[c.ch] + c.device_dropped;
}

// The LIN ops below may be called from the UI; like iface_send they need the interface open.
void linde_sleep_wakeup(Iface& iface, bool wakeup)
{
    auto& c = iface_impl<LindeChannel>(iface);
    control_out(*c.dev, LIN_USB_BREQ_SLEEP_WAKEUP, c.ch,
                lin_usb_sleep_wakeup_t{.command = wakeup ? uint8_t{1} : uint8_t{0}});
}

void linde_set_schedule(Iface& iface, uint8_t table)
{
    auto& c = iface_impl<LindeChannel>(iface);
    if (table >= max_tables)
    {
        log_warning(std::format("LindeAPI: schedule table {} out of range", table));
        return;
    }
    set_mode(*c.dev, c.ch, LIN_USB_MODE_STOP);
    if (c.table_entry_counts[table] > 0)
    {
        schedule_start(*c.dev, c.ch, table, c.table_entry_counts[table]);
    }
}

void linde_diag_request(Iface& iface, uint8_t nad, std::span<const uint8_t> data)
{
    auto& c = iface_impl<LindeChannel>(iface);
    lin_usb_host_frame_t frame{.lin_id = 0x3C, .channel = c.ch, .dlc = 8}; // MasterReq
    frame.data[0] = nad;
    std::copy_n(data.begin(), std::min<std::size_t>(data.size(), 7), frame.data + 1);
    send_frame(*c.dev, frame);
}

} // namespace

void linde_frame_to_message(const lin_usb_host_frame_t& f, BusMessage& m) noexcept
{
    m = BusMessage{
        // The firmware reports the protected id (parity in bits 6/7); BusMessage carries the plain id.
        .id = f.lin_id & 0x3Fu,
        .type = BusType::LIN,
    };
    set_length(m, std::min<int>(f.dlc, 8));
    std::memcpy(m.data.data(), f.data, m.len);
    // Every frame arrives with echo_id RX; SUBSCRIBER clear = this node put the data on the bus.
    if (!(f.flags & LIN_USB_FRAME_FLAG_SUBSCRIBER))
    {
        m.flags |= bus_flag::tx;
    }
    if (f.flags & LIN_USB_FRAME_FLAG_SLEEP)
    {
        m.flags |= bus_flag::lin_sleep;
    }
    if (f.flags & LIN_USB_FRAME_FLAG_WAKEUP)
    {
        m.flags |= bus_flag::lin_wakeup;
    }
    if (f.flags & LIN_USB_FRAME_FLAG_ERROR)
    {
        // An unanswered frame also leaves VALID clear, so only an answered one has a checksum error.
        if (!(f.flags & LIN_USB_FRAME_FLAG_RESPONDED))
        {
            m.errors = bus_error::lin_not_responded;
        }
        else if (!(f.flags & LIN_USB_FRAME_FLAG_VALID))
        {
            m.errors = bus_error::lin_checksum_error;
        }
        else
        {
            m.errors = bus_error::generic;
        }
    }
}

extern const DriverOps linde_driver = {
    .name = "LindeAPI",
    .enumerate = linde_enumerate,
    .open = linde_open,
    .close = linde_close,
    .send = linde_send,
    .read = linde_read,
    .stats = linde_stats,
    .lin_sleep_wakeup = linde_sleep_wakeup,
    .lin_set_schedule = linde_set_schedule,
    .lin_diag_request = linde_diag_request,
};
