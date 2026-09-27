/*

  Copyright (c) 2015, 2016 Hubert Denkmair <hubert@denkmair.de>
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

// DBC text -> CanDb. Handles VERSION, NS_, BS_, BU_, BO_/SG_, CM_, VAL_ and SIG_VALTYPE_;
// other sections are skipped, so a non-DBC file "parses" into an empty database - check
// db.messages, not only the return value. Errors are logged via core/log and, when
// `errors` is given, collected with their line: a bad statement is skipped and parsing
// continues, so one pass reports every problem. The return value is false as soon as
// anything failed.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "db/model/can_db.h"

struct DbcError
{
    int line = 0;          // 1-based line in the DBC text, 0 = not tied to a line (semantic check)
    std::string message;   // e.g. "BO_ 256: expected message name"
};

// `text` is read as Latin-1 (as before the port); strings are stored as UTF-8.
[[nodiscard]] bool dbc_parse(std::string_view text, CanDb& db, std::vector<DbcError>* errors = nullptr);
// Sets db.path once the file could be read, like DbcParser::parseFile did.
[[nodiscard]] bool dbc_parse_file(const std::filesystem::path& path, CanDb& db, std::vector<DbcError>* errors = nullptr);
