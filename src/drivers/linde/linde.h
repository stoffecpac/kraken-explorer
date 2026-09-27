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

// LindeAPI driver (lin_usb LIN adapter): `extern const DriverOps linde_driver` in linde.cpp.
// Only the wire-to-BusMessage conversion is exposed, for the unit test.

#pragma once

#include "core/bus_message.h"
#include "drivers/linde/lin_usb_protocol.h"

// Converts a device->host bus frame (not a SET_DATA_ACK) to a LIN BusMessage.
// Sets everything except iface and ts_ns.
void linde_frame_to_message(const lin_usb_host_frame_t& frame, BusMessage& m) noexcept;
