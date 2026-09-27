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

// One vendor-specific interface (class/subclass 0xFF, identified by its
// bInterfaceProtocol) of a composite USB device, opened on its own.
//
// Implemented with libusb (usb_vendor_libusb.cpp).
//
// Transfers may be issued from several threads at once (a bulk reader plus
// control requests); callers serialise transfers on the same endpoint.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>

struct libusb_context;
struct libusb_device_handle;

enum class UsbStatus : uint8_t
{
    Ok,
    Timeout,
    Error,
};

struct UsbVendorId
{
    uint16_t vid;
    uint16_t pid;
    uint8_t protocol; // bInterfaceProtocol
};

// Not copyable or movable (mutex). Closed with usb_vendor_close().
struct UsbVendor
{
    uint8_t itf = 0;
    uint8_t ep_in = 0;
    uint8_t ep_out = 0;

    std::mutex error_mutex;   // guards last_error
    std::string last_error;

    libusb_context* ctx = nullptr;
    libusb_device_handle* handle = nullptr;
    bool kernel_driver_detached = false;
};

// Number of attached devices exposing the interface.
[[nodiscard]] int usb_vendor_count(const UsbVendorId& id);

// Opens the index-th device exposing the interface and claims it.
[[nodiscard]] bool usb_vendor_open(UsbVendor& usb, const UsbVendorId& id, int index);
void usb_vendor_close(UsbVendor& usb);

UsbStatus usb_vendor_bulk_read(UsbVendor& usb, void* data, int len, int& transferred, unsigned timeout_ms);
UsbStatus usb_vendor_bulk_write(UsbVendor& usb, const void* data, int len, unsigned timeout_ms);

// Vendor requests addressed to this interface (wIndex = interface number).
UsbStatus usb_vendor_control_in(UsbVendor& usb, uint8_t request, uint16_t value, void* data, uint16_t len,
                                unsigned timeout_ms);
UsbStatus usb_vendor_control_out(UsbVendor& usb, uint8_t request, uint16_t value, const void* data, uint16_t len,
                                 unsigned timeout_ms);

// Description of the most recent failure.
[[nodiscard]] inline std::string usb_vendor_last_error(UsbVendor& usb)
{
    std::lock_guard lock(usb.error_mutex);
    return usb.last_error;
}

// Records "context: reason" as the last error and returns status.
inline UsbStatus usb_vendor_fail(UsbVendor& usb, UsbStatus status, const std::string& context,
                                 const std::string& reason)
{
    std::lock_guard lock(usb.error_mutex);
    usb.last_error = context + ": " + reason;
    return status;
}
