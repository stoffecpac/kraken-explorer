/*
  Copyright (c) 2026 Schildkroet

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

// Kvaser CANlib (linuxcan), classic CAN only.
// Built with -DCANGAROO_KVASER=ON.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "drivers/driver.h"

#include <canlib.h>

namespace
{

// Device timestamps count from canBusOn, 1 us per tick on linuxcan.
constexpr int64_t kvaser_tick_ns = 1000;

struct Kvaser
{
    canHandle handle = -1;
    int64_t open_ns = 0;   // host time at canBusOn, base for device timestamps

    std::mutex tx_mutex;
    std::deque<BusMessage> tx_done;   // sent frames, reported by read()

    std::atomic<uint64_t> rx_frames{0};
    std::atomic<uint64_t> rx_errors{0};
    std::atomic<uint64_t> tx_frames{0};
    std::atomic<uint64_t> tx_errors{0};
};

std::string kvaser_channel_name(int ch)
{
    char dev[256] = {};
    canGetChannelData(ch, canCHANNELDATA_DEVDESCR_ASCII, dev, sizeof(dev));
    return std::format("{} (ch{})", dev, ch);
}

void kvaser_enumerate(std::vector<IfaceInfo>& out)
{
    static std::once_flag init;
    std::call_once(init, [] { canInitializeLibrary(); });

    int channels = 0;
    if (const canStatus st = canGetNumberOfChannels(&channels); st != canOK)
    {
        log_error(std::format("Kvaser: canGetNumberOfChannels failed: {}", static_cast<int>(st)));
        return;
    }
    std::vector<CanTiming> bitrates;
    for (unsigned br : {10000u, 20000u, 50000u, 83333u, 100000u, 125000u, 250000u, 500000u, 800000u, 1000000u})
    {
        bitrates.push_back({.bitrate = br});
    }
    for (int ch = 0; ch < channels; ++ch)
    {
        out.push_back({
            .name = kvaser_channel_name(ch),
            .details = std::format("Kvaser channel {}", ch),
            .version = "",
            .capabilities = iface_cap::listen_only,
            .bitrates = bitrates,
        });
    }
}

long kvaser_bitrate(unsigned bitrate)
{
    switch (bitrate)
    {
    case 10000:   return canBITRATE_10K;
    case 50000:   return canBITRATE_50K;
    case 62500:   return canBITRATE_62K;
    case 83333:   return canBITRATE_83K;
    case 100000:  return canBITRATE_100K;
    case 125000:  return canBITRATE_125K;
    case 250000:  return canBITRATE_250K;
    case 1000000: return canBITRATE_1M;
    default:
        // ponytail: 20k/800k have no canBITRATE_* constant, would need explicit tseg values
        if (bitrate != 500000)
        {
            log_warning(std::format("Kvaser: bitrate {} not supported, using 500000", bitrate));
        }
        return canBITRATE_500K;
    }
}

bool kvaser_open(Iface& iface, const IfaceConfig& config)
{
    const std::string& name = iface.info.name;
    int channels = 0;
    canGetNumberOfChannels(&channels);
    int ch = 0;
    while (ch < channels && kvaser_channel_name(ch) != name)
    {
        ++ch;
    }
    if (ch == channels)
    {
        log_error(std::format("Kvaser: channel {} is gone", name));
        return false;
    }

    const canHandle h = canOpenChannel(ch, canOPEN_ACCEPT_VIRTUAL);
    if (h < 0)
    {
        log_error(std::format("Kvaser {}: canOpenChannel failed: {}", name, static_cast<int>(h)));
        return false;
    }
    if (config.configure)
    {
        if (const canStatus st = canSetBusParams(h, kvaser_bitrate(config.bitrate), 0, 0, 0, 0, 0); st != canOK)
        {
            log_error(std::format("Kvaser {}: canSetBusParams failed: {}", name, static_cast<int>(st)));
            canClose(h);
            return false;
        }
        canSetBusOutputControl(h, config.listen_only ? canDRIVER_SILENT : canDRIVER_NORMAL);
    }
    if (const canStatus st = canBusOn(h); st != canOK)
    {
        log_error(std::format("Kvaser {}: canBusOn failed: {}", name, static_cast<int>(st)));
        canClose(h);
        return false;
    }

    auto k = std::make_unique<Kvaser>();
    k->handle = h;
    k->open_ns = now_ns();
    iface_set_impl(iface, std::move(k));
    return true;
}

void kvaser_close(Iface& iface)
{
    if (auto* k = static_cast<Kvaser*>(iface.impl.get()); k && k->handle >= 0)
    {
        canBusOff(k->handle);
        canClose(k->handle);
        k->handle = -1;
    }
}

bool kvaser_send(Iface& iface, const BusMessage& msg)
{
    auto& k = iface_impl<Kvaser>(iface);
    unsigned flags = has_flag(msg, bus_flag::extended) ? canMSG_EXT : canMSG_STD;
    if (has_flag(msg, bus_flag::rtr))
    {
        flags |= canMSG_RTR;
    }
    const unsigned dlc = std::min<unsigned>(msg.len, 8);
    uint8_t data[8] = {};
    if (!has_flag(msg, bus_flag::rtr))
    {
        std::copy_n(msg.data.begin(), dlc, data);
    }
    if (const canStatus st = canWrite(k.handle, static_cast<long>(can_id(msg)), data, dlc, flags); st != canOK)
    {
        log_error(std::format("Kvaser {}: canWrite failed: {}", iface.info.name, static_cast<int>(st)));
        ++k.tx_errors;
        return false;
    }
    ++k.tx_frames;
    BusMessage tx = msg;
    tx.iface = iface.index;
    tx.flags |= bus_flag::tx;
    tx.ts_ns = now_ns();
    std::lock_guard lock(k.tx_mutex);
    k.tx_done.push_back(tx);
    return true;
}

int kvaser_read(Iface& iface, BusMessage* out, int max, int timeout_ms)
{
    auto& k = iface_impl<Kvaser>(iface);
    int n = 0;
    {
        std::lock_guard lock(k.tx_mutex);
        while (n < max && !k.tx_done.empty())
        {
            out[n++] = k.tx_done.front();
            k.tx_done.pop_front();
        }
    }
    // ponytail: a send during the wait is reported after the timeout (<= 100 ms late); shorter wait if TX echo latency matters.
    unsigned long wait = n > 0 ? 0 : static_cast<unsigned long>(timeout_ms);
    while (n < max)
    {
        long id = 0;
        uint8_t data[8] = {};
        unsigned dlc = 0;
        unsigned flags = 0;
        unsigned long ts = 0;
        const canStatus st = canReadWait(k.handle, &id, data, &dlc, &flags, &ts, wait);
        wait = 0;
        if (st == canERR_NOMSG || st == canERR_TIMEOUT)
        {
            break;
        }
        if (st != canOK)
        {
            log_error(std::format("Kvaser {}: canReadWait failed: {}", iface.info.name, static_cast<int>(st)));
            return n > 0 ? n : -1;
        }
        if (flags & canMSG_ERROR_FRAME)
        {
            ++k.rx_errors;
            continue;
        }
        BusMessage& m = out[n++];
        m = BusMessage{.id = static_cast<uint32_t>(id) & can_id_mask_extended, .iface = iface.index};
        if (flags & canMSG_EXT)
        {
            m.flags |= bus_flag::extended;
        }
        if (flags & canMSG_RTR)
        {
            m.flags |= bus_flag::rtr;
        }
        m.ts_ns = k.open_ns + static_cast<int64_t>(ts) * kvaser_tick_ns;
        set_length(m, static_cast<int>(std::min(dlc, 8u)));
        std::copy_n(data, m.len, m.data.begin());
        ++k.rx_frames;
    }
    return n;
}

void kvaser_stats(Iface& iface, IfaceStats& out)
{
    auto& k = iface_impl<Kvaser>(iface);
    out = {
        .state = IfaceState::Ok,
        .rx_frames = k.rx_frames,
        .rx_errors = k.rx_errors,
        .tx_frames = k.tx_frames,
        .tx_errors = k.tx_errors,
    };
    unsigned long flags = 0;
    canRequestChipStatus(k.handle);
    canReadStatus(k.handle, &flags);
    if (flags & canSTAT_BUS_OFF)
    {
        out.state = IfaceState::BusOff;
    }
    else if (flags & canSTAT_ERROR_PASSIVE)
    {
        out.state = IfaceState::Passive;
    }
    else if (flags & canSTAT_ERROR_WARNING)
    {
        out.state = IfaceState::Warning;
    }
}

} // namespace

extern const DriverOps kvaser_driver = {
    .name = "Kvaser",
    .enumerate = kvaser_enumerate,
    .open = kvaser_open,
    .close = kvaser_close,
    .send = kvaser_send,
    .read = kvaser_read,
    .stats = kvaser_stats,
};
