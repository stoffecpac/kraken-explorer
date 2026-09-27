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

#pragma once

// Complete trace files in every export format, written from a plain list of
// messages to a std::ostream (open it in binary mode). Free of App so whole files
// can be unit-tested and checked against reference readers.

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <span>
#include <string>

#include "core/bus_message.h"
#include "core/trace_file_format.h"

// Interface label for candump lines and pcapng interface descriptions.
using IfaceNameFn = std::function<std::string(uint16_t iface)>;

void write_trace_file(std::ostream& out, TraceFileFormat format, std::span<const BusMessage> messages,
                      const IfaceNameFn& iface_name);

