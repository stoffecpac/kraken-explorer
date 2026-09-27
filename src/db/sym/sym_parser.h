// PEAK PCAN Symbol Editor file (.sym, FormatVersion 5.0/6.0) -> CanDb, the same model
// the DBC parser fills. Handles {ENUMS}, {SIGNALS}, {SEND}/{RECEIVE}/{SENDRECEIVE}
// symbols (ID, Type, Len, Mux, Sig, Var); {VIRTUALVARS} and unknown sections are skipped.
// Semantics follow cantools' sym.py. Errors are logged via core/log.

#pragma once

#include <filesystem>
#include <string_view>

#include "db/model/can_db.h"

// `text` is UTF-8, or Latin-1 when it is not valid UTF-8; strings are stored as UTF-8.
[[nodiscard]] bool sym_parse(std::string_view text, CanDb& db);
// Sets db.path on success.
[[nodiscard]] bool sym_parse_file(const std::filesystem::path& path, CanDb& db);
// By extension: .sym -> sym_parse_file, anything else -> dbc_parse_file.
[[nodiscard]] bool can_db_parse_file(const std::filesystem::path& path, CanDb& db);
