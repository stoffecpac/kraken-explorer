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

// CanDb -> DBC text, the inverse of db/dbc/dbc_parser: what dbc_write() emits, dbc_parse()
// reads back into an identical database (VERSION, NS_, BS_, BU_, BO_/SG_, CM_, VAL_,
// SIG_VALTYPE_). Messages in raw-id order, Motorola start bits back in DBC numbering.

#pragma once

#include <filesystem>
#include <ostream>
#include <string>

#include "db/model/can_db.h"

void dbc_write(const CanDb& db, std::ostream& out);
// false with `*error` set (when non-null) if the file cannot be opened or written.
[[nodiscard]] bool dbc_write_file(const CanDb& db, const std::filesystem::path& path, std::string* error);
