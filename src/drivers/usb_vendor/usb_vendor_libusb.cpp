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

#include "drivers/usb_vendor/usb_vendor.h"

#include <string>
#include <vector>

#include <libusb.h>

namespace
{

constexpr uint8_t request_vendor_itf_out = 0x41u; // vendor | interface | host->device
constexpr uint8_t request_vendor_itf_in = 0xC1u;  // vendor | interface | device->host

// True if dev matches id; fills the interface number and bulk endpoints.
bool find_interface(libusb_device* dev, const UsbVendorId& id, uint8_t& itf, uint8_t& ep_in, uint8_t& ep_out)
{
    libusb_device_descriptor desc{};
    if (libusb_get_device_descriptor(dev, &desc) != 0)
    {
        return false;
    }
    if (desc.idVendor != id.vid || desc.idProduct != id.pid)
    {
        return false;
    }

    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0)
    {
        return false;
    }

    bool found = false;
    for (uint8_t i = 0; i < cfg->bNumInterfaces && !found; i++)
    {
        const libusb_interface& ifc = cfg->interface[i];
        for (int a = 0; a < ifc.num_altsetting && !found; a++)
        {
            const libusb_interface_descriptor& alt = ifc.altsetting[a];
            if (alt.bInterfaceClass != LIBUSB_CLASS_VENDOR_SPEC || alt.bInterfaceSubClass != 0xFFu
                || alt.bInterfaceProtocol != id.protocol)
            {
                continue;
            }
            itf = alt.bInterfaceNumber;
            for (uint8_t e = 0; e < alt.bNumEndpoints; e++)
            {
                const libusb_endpoint_descriptor& ep = alt.endpoint[e];
                if ((ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
                {
                    continue;
                }
                if ((ep.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN)
                {
                    ep_in = ep.bEndpointAddress;
                }
                else
                {
                    ep_out = ep.bEndpointAddress;
                }
            }
            found = true;
        }
    }
    libusb_free_config_descriptor(cfg);
    return found;
}

// One device exposing the interface, with the interface number and bulk endpoints.
struct UsbMatch
{
    libusb_device* dev = nullptr;
    uint8_t itf = 0;
    uint8_t ep_in = 0;
    uint8_t ep_out = 0;
};

// The devices in list exposing the interface. The VID/PID may be shared with devices
// lacking it, so only these count towards an index.
std::vector<UsbMatch> matching_devices(libusb_device** list, ssize_t cnt, const UsbVendorId& id)
{
    std::vector<UsbMatch> matches;
    for (ssize_t i = 0; i < cnt; i++)
    {
        UsbMatch m{.dev = list[i]};
        if (find_interface(m.dev, id, m.itf, m.ep_in, m.ep_out))
        {
            matches.push_back(m);
        }
    }
    return matches;
}

UsbStatus to_status(int rc)
{
    switch (rc)
    {
    case LIBUSB_SUCCESS:
        return UsbStatus::Ok;
    case LIBUSB_ERROR_TIMEOUT:
        return UsbStatus::Timeout;
    default:
        return UsbStatus::Error;
    }
}

std::string error_text(int rc)
{
    return libusb_strerror(static_cast<libusb_error>(rc));
}

} // namespace

int usb_vendor_count(const UsbVendorId& id)
{
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0)
    {
        return 0;
    }

    libusb_device** list = nullptr;
    const ssize_t cnt = libusb_get_device_list(ctx, &list);
    const int matches = static_cast<int>(matching_devices(list, cnt, id).size());
    if (cnt >= 0)
    {
        libusb_free_device_list(list, 1);
    }
    libusb_exit(ctx);
    return matches;
}

bool usb_vendor_open(UsbVendor& usb, const UsbVendorId& id, int index)
{
    usb_vendor_close(usb);

    // Own context per handle so open/close of one device never affects another.
    if (libusb_init(&usb.ctx) != 0)
    {
        usb.ctx = nullptr;
        usb_vendor_fail(usb, UsbStatus::Error, "libusb_init", "failed");
        return false;
    }

    libusb_device** list = nullptr;
    const ssize_t cnt = libusb_get_device_list(usb.ctx, &list);
    if (cnt < 0)
    {
        usb_vendor_fail(usb, UsbStatus::Error, "libusb_get_device_list", error_text(static_cast<int>(cnt)));
        usb_vendor_close(usb);
        return false;
    }

    const std::vector<UsbMatch> matches = matching_devices(list, cnt, id);
    libusb_device* found = nullptr;
    if (index >= 0 && static_cast<std::size_t>(index) < matches.size())
    {
        const UsbMatch& m = matches[static_cast<std::size_t>(index)];
        found = m.dev;
        usb.itf = m.itf;
        usb.ep_in = m.ep_in;
        usb.ep_out = m.ep_out;
    }

    int rc = found ? libusb_open(found, &usb.handle) : LIBUSB_ERROR_NOT_FOUND;
    libusb_free_device_list(list, 1);
    if (!found)
    {
        usb_vendor_fail(usb, UsbStatus::Error, "open",
                        "no device with this interface at index " + std::to_string(index));
        usb_vendor_close(usb);
        return false;
    }
    if (rc != 0)
    {
        usb.handle = nullptr;
        if (rc == LIBUSB_ERROR_ACCESS)
        {
            usb_vendor_fail(usb, UsbStatus::Error, "open",
                            "permission denied. Add a udev rule (see README) or run as root.");
        }
        else
        {
            usb_vendor_fail(usb, to_status(rc), "libusb_open", error_text(rc));
        }
        usb_vendor_close(usb);
        return false;
    }

    if (libusb_kernel_driver_active(usb.handle, usb.itf) == 1)
    {
        rc = libusb_detach_kernel_driver(usb.handle, usb.itf);
        if (rc != 0)
        {
            usb_vendor_fail(usb, to_status(rc), "libusb_detach_kernel_driver", error_text(rc));
            usb_vendor_close(usb);
            return false;
        }
        usb.kernel_driver_detached = true;
    }

    rc = libusb_claim_interface(usb.handle, usb.itf);
    if (rc != 0)
    {
        usb_vendor_fail(usb, to_status(rc), "libusb_claim_interface", error_text(rc));
        usb_vendor_close(usb);
        return false;
    }
    return true;
}

void usb_vendor_close(UsbVendor& usb)
{
    if (usb.handle)
    {
        libusb_release_interface(usb.handle, usb.itf);
        if (usb.kernel_driver_detached)
        {
            libusb_attach_kernel_driver(usb.handle, usb.itf);
        }
        libusb_close(usb.handle);
        usb.handle = nullptr;
    }
    if (usb.ctx)
    {
        libusb_exit(usb.ctx);
        usb.ctx = nullptr;
    }
    usb.kernel_driver_detached = false;
    usb.itf = 0;
    usb.ep_in = 0;
    usb.ep_out = 0;
}

UsbStatus usb_vendor_bulk_read(UsbVendor& usb, void* data, int len, int& transferred, unsigned timeout_ms)
{
    transferred = 0;
    if (!usb.handle || !usb.ep_in)
    {
        return usb_vendor_fail(usb, UsbStatus::Error, "bulk IN", "device not open");
    }
    const int rc = libusb_bulk_transfer(usb.handle, usb.ep_in, static_cast<unsigned char*>(data), len,
                                        &transferred, timeout_ms);
    // Timeouts are routine for the polling reader: not an error to report.
    if (rc == LIBUSB_ERROR_TIMEOUT)
    {
        return UsbStatus::Timeout;
    }
    return rc == 0 ? UsbStatus::Ok : usb_vendor_fail(usb, to_status(rc), "bulk IN", error_text(rc));
}

UsbStatus usb_vendor_bulk_write(UsbVendor& usb, const void* data, int len, unsigned timeout_ms)
{
    if (!usb.handle || !usb.ep_out)
    {
        return usb_vendor_fail(usb, UsbStatus::Error, "bulk OUT", "device not open");
    }
    int transferred = 0;
    // libusb takes a non-const buffer but does not modify it for OUT transfers.
    auto* buf = static_cast<unsigned char*>(const_cast<void*>(data));
    const int rc = libusb_bulk_transfer(usb.handle, usb.ep_out, buf, len, &transferred, timeout_ms);
    if (rc != 0)
    {
        return usb_vendor_fail(usb, to_status(rc), "bulk OUT", error_text(rc));
    }
    if (transferred != len)
    {
        return usb_vendor_fail(usb, UsbStatus::Error, "bulk OUT", "short write");
    }
    return UsbStatus::Ok;
}

UsbStatus usb_vendor_control_in(UsbVendor& usb, uint8_t request, uint16_t value, void* data, uint16_t len,
                                unsigned timeout_ms)
{
    if (!usb.handle)
    {
        return usb_vendor_fail(usb, UsbStatus::Error, "control IN", "device not open");
    }
    const int rc = libusb_control_transfer(usb.handle, request_vendor_itf_in, request, value, usb.itf,
                                           static_cast<unsigned char*>(data), len, timeout_ms);
    return rc < 0 ? usb_vendor_fail(usb, to_status(rc), "control IN", error_text(rc)) : UsbStatus::Ok;
}

UsbStatus usb_vendor_control_out(UsbVendor& usb, uint8_t request, uint16_t value, const void* data, uint16_t len,
                                 unsigned timeout_ms)
{
    if (!usb.handle)
    {
        return usb_vendor_fail(usb, UsbStatus::Error, "control OUT", "device not open");
    }
    auto* buf = static_cast<unsigned char*>(const_cast<void*>(data));
    const int rc = libusb_control_transfer(usb.handle, request_vendor_itf_out, request, value, usb.itf, buf, len,
                                           timeout_ms);
    return rc < 0 ? usb_vendor_fail(usb, to_status(rc), "control OUT", error_text(rc)) : UsbStatus::Ok;
}
