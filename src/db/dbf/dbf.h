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

// BUSMASTER CAN database (.dbf, database version 1.3) <-> CanDb, the model the DBC parser fills.
// Format from BUSMASTER's own reader/writer (Application/MsgSignal.cpp) and its DBC<->DBF
// converters: one tag per line, comma-separated fields.
//
//   [START_MSG] name,id,length,signal count,1,S|X[,sender]
//   [START_SIGNALS] name,length,byte (1-based),bit in byte,U|I|B|F|D,raw max,raw min,1 Intel|0 Motorola,
//                   offset,factor,unit[,M|m<n>,receivers...]
//   [VALUE_DESCRIPTION] "text",value
//   [END_MSG]
//
// (byte, bit) is the signal's least significant bit for both byte orders; a Motorola signal grows
// toward lower bytes. Min / max are raw values. Comments go to [START_DESC_MSG] / [START_DESC_SIG]
// as `id S|X "text";` / `id S|X signal "text";`. The writer emits what BUSMASTER loads (the 1.3
// header, an exact message count); the reader also takes the app's unquoted value descriptions.
// ponytail: value tables, attributes and J1939 PGN ids are skipped; add them if a file needs them.

#pragma once

#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>

#include "db/model/can_db.h"

[[nodiscard]] bool dbf_parse(std::string_view text, CanDb& db, std::string* error = nullptr);
// Sets db.path on success.
[[nodiscard]] bool dbf_parse_file(const std::filesystem::path& path, CanDb& db, std::string* error = nullptr);

void dbf_write(const CanDb& db, std::ostream& out);
[[nodiscard]] bool dbf_write_file(const CanDb& db, const std::filesystem::path& path, std::string* error);
