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

// Host side of the aio_usb GPIO/analog interface (bInterfaceProtocol 0x02) on the
// candleLight-family composite device, driven by the GPIO Control window.
// A poll thread reads AIO_USB_BREQ_READ_STATUS every cycle; the UI reads the latest
// report each frame with aiode_state() (replaces GpioProvider::gpioUpdated).

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "drivers/aiode/aio_usb_protocol.h"
#include "drivers/usb_vendor/usb_vendor.h"

// Latest state report.
struct AiodeState
{
    uint16_t pins = 0;                    // digital levels, bit set = high
    std::array<uint16_t, 16> analog{};    // aiode_analog_pin_count() entries valid
    uint64_t reports = 0;                 // count of reports received, 0 = none yet
};

// Not movable (mutex, thread): aiode_scan() returns them by unique_ptr.
struct Aiode
{
    std::string name;             // "Aiode Device N"
    UsbVendor usb;
    aio_usb_caps_t caps{};
    void (*wake)() = nullptr;     // called after each report (e.g. glfwPostEmptyEvent)

    std::mutex usb_mutex;         // control transfers: poll thread vs. UI
    std::atomic<uint16_t> dir_mask{0};   // bit set = output line
    std::atomic<uint16_t> out_mask{0};   // last requested output levels
    std::atomic<uint32_t> cycle_ms{50};

    std::mutex state_mutex;
    AiodeState state;

    std::jthread poll;
};

// Opens every connected aiode device. Call aiode_close() on each before dropping it.
[[nodiscard]] std::vector<std::unique_ptr<Aiode>> aiode_scan();
void aiode_close(Aiode& a);

[[nodiscard]] int aiode_digital_pin_count(const Aiode& a) noexcept;
[[nodiscard]] int aiode_analog_pin_count(const Aiode& a) noexcept;
// Report interval limits for the UI (as GpioProvider::maxCycleMs).
inline constexpr int aiode_min_cycle_ms = 5;
inline constexpr int aiode_max_cycle_ms = 500;

// Enables/disables polling; cycle_ms is clamped to >= aiode_min_cycle_ms,
// dir_mask sets the line directions (bit set = output). UI thread.
void aiode_set_config(Aiode& a, bool enable, uint16_t cycle_ms, uint16_t dir_mask);
// Drives the output lines (bit set = high). UI thread.
void aiode_set_output(Aiode& a, uint16_t output_mask);
// Copy of the latest report.
[[nodiscard]] AiodeState aiode_state(Aiode& a);
