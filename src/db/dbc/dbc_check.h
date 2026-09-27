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

// Semantic checks of an in-memory CanDb, independent of how it was loaded: duplicate
// names, signal bits outside the payload or overlapping another signal (multiplexed
// signals may overlap when their mux values differ), muxed signals without a muxer,
// factor 0, min > max, float signals of the wrong length. Every finding has line = 0
// and names the message / signal, e.g. "EngineData.RPM: bits 40..55 overlap Torque (bits 48..63)".

#pragma once

#include <vector>

#include "db/dbc/dbc_parser.h"
#include "db/model/can_db.h"

[[nodiscard]] std::vector<DbcError> dbc_check(const CanDb& db);
