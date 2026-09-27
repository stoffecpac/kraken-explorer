/*

  Copyright (c) 2026 Schildkroet

  This file is part of CANgaroo.

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

#include "drivers/aiode/aiode.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <format>
#include <mutex>

#include "core/log.h"

namespace
{

constexpr unsigned ctrl_timeout_ms = 1000;

constexpr UsbVendorId usb_id{
    .vid = AIO_USB_VID,
    .pid = AIO_USB_PID,
    .protocol = AIO_USB_ITF_PROTOCOL,
};

template <class T>
bool control_out(Aiode& a, uint8_t breq, uint16_t value, const T& data)
{
    return usb_vendor_control_out(a.usb, breq, value, &data, sizeof(T), ctrl_timeout_ms) == UsbStatus::Ok;
}

template <class T>
bool control_in(Aiode& a, uint8_t breq, T& data)
{
    data = {};
    return usb_vendor_control_in(a.usb, breq, 0, &data, sizeof(T), ctrl_timeout_ms) == UsbStatus::Ok;
}

bool aiode_open(Aiode& a, int index)
{
    if (!usb_vendor_open(a.usb, usb_id, index))
    {
        return false;
    }
    control_out(a, AIO_USB_BREQ_HOST_FORMAT, 0, aio_usb_host_config_t{.byte_order = 0x0000beefu});
    if (!control_in(a, AIO_USB_BREQ_DEVICE_CONFIG, a.caps))
    {
        usb_vendor_close(a.usb);
        return false;
    }
    a.name = std::format("Aiode Device {}", index + 1);
    return true;
}

void poll_loop(std::stop_token stop, Aiode& a)
{
    std::mutex sleep_mutex; // only for the wait below; nothing else locks it
    std::condition_variable_any sleep_cv;
    while (!stop.stop_requested())
    {
        aio_usb_report_t report{};
        bool ok = false;
        {
            std::lock_guard lock(a.usb_mutex);
            ok = control_in(a, AIO_USB_BREQ_READ_STATUS, report);
        }
        if (ok)
        {
            {
                std::lock_guard lock(a.state_mutex);
                a.state.pins = static_cast<uint16_t>(report.io_states & 0xFFFFu);
                std::memcpy(a.state.analog.data(), report.analog, sizeof(a.state.analog)); // packed source
                ++a.state.reports;
            }
            if (a.wake)
            {
                a.wake();
            }
        }
        // One cycle, or until a disable request wakes us.
        std::unique_lock lock(sleep_mutex);
        sleep_cv.wait_for(lock, stop, std::chrono::milliseconds(a.cycle_ms), [] { return false; });
    }
}

} // namespace

std::vector<std::unique_ptr<Aiode>> aiode_scan()
{
    std::vector<std::unique_ptr<Aiode>> result;
    const int count = usb_vendor_count(usb_id);
    for (int i = 0; i < count; ++i)
    {
        auto a = std::make_unique<Aiode>();
        if (aiode_open(*a, i))
        {
            result.push_back(std::move(a));
        }
        else
        {
            log_warning(std::format("aiode: cannot open device {}: {}", i, usb_vendor_last_error(a->usb)));
        }
    }
    return result;
}

void aiode_close(Aiode& a)
{
    a.poll = {};
    usb_vendor_close(a.usb);
}

int aiode_digital_pin_count(const Aiode& a) noexcept
{
    return std::min<int>(16, a.caps.io_count);
}

int aiode_analog_pin_count(const Aiode& a) noexcept
{
    return (a.caps.features & AIO_USB_FEATURE_ANALOG) ? std::min<int>(16, a.caps.analog_count) : 0;
}

void aiode_set_config(Aiode& a, bool enable, uint16_t cycle_ms, uint16_t dir_mask)
{
    // Stop polling first so reconfiguration owns the USB exclusively.
    a.poll = {};
    a.dir_mask = dir_mask;
    a.cycle_ms = std::max<uint32_t>(aiode_min_cycle_ms, cycle_ms);
    if (!enable)
    {
        return;
    }
    {
        std::lock_guard lock(a.usb_mutex);
        for (int line = 0; line < aiode_digital_pin_count(a); ++line)
        {
            const aio_usb_io_config_t cfg{
                .mode = ((dir_mask >> line) & 1u) ? uint8_t{AIO_USB_IO_MODE_OUTPUT} : uint8_t{AIO_USB_IO_MODE_INPUT},
                .auto_report_ms = 0, // host polls READ_STATUS instead
            };
            control_out(a, AIO_USB_BREQ_IO_CONFIG, static_cast<uint16_t>(line), cfg);
        }
        // Apply the requested output levels to the freshly configured outputs.
        control_out(a, AIO_USB_BREQ_IO_SET, 0, aio_usb_io_set_t{.mask = dir_mask, .values = static_cast<uint32_t>(a.out_mask & dir_mask)});
    }
    a.poll = std::jthread(poll_loop, std::ref(a));
}

void aiode_set_output(Aiode& a, uint16_t output_mask)
{
    a.out_mask = output_mask;
    const uint16_t dir = a.dir_mask;
    std::lock_guard lock(a.usb_mutex);
    control_out(a, AIO_USB_BREQ_IO_SET, 0, aio_usb_io_set_t{.mask = dir, .values = static_cast<uint32_t>(output_mask & dir)});
}

AiodeState aiode_state(Aiode& a)
{
    std::lock_guard lock(a.state_mutex);
    return a.state;
}
