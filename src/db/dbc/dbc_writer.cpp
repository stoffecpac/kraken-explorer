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

#include "dbc_writer.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

namespace
{

constexpr std::string_view header = R"(VERSION ""


NS_ : 
	NS_DESC_
	CM_
	BA_DEF_
	BA_
	VAL_
	CAT_DEF_
	CAT_
	FILTER
	BA_DEF_DEF_
	EV_DATA_
	ENVVAR_DATA_
	SGTYPE_
	SGTYPE_VAL_
	BA_DEF_SGTYPE_
	BA_SGTYPE_
	SIG_TYPE_REF_
	VAL_TABLE_
	SIG_GROUP_
	SIG_VALTYPE_
	SIG_UNIT_
	SG_MUL_VAL_

BS_:

BU_:)";

constexpr std::string_view no_node = "Vector__XXX";

// Shortest text that reads back to the same double. The parser's number token only
// accepts an upper-case exponent marker.
void append_number(std::string& out, double v)
{
    const size_t begin = out.size();
    std::format_to(std::back_inserter(out), "{}", v);
    std::replace(out.begin() + static_cast<std::ptrdiff_t>(begin), out.end(), 'e', 'E');
}

// "..." with '\' and '"' escaped and UTF-8 narrowed to Latin-1, the inverse of the
// parser (which reads the file as Latin-1 and widens every byte >= 0x80).
// ponytail: code points above U+00FF have no Latin-1 byte and are passed through as
// UTF-8, which the parser then reads as two Latin-1 characters; DBC has no encoding rule.
void append_quoted(std::string& out, std::string_view s)
{
    out.push_back('"');
    for (size_t i = 0; i < s.size(); ++i)
    {
        const auto c = static_cast<unsigned char>(s[i]);
        const auto next = i + 1 < s.size() ? static_cast<unsigned char>(s[i + 1]) : 0u;
        if ((c == 0xC2 || c == 0xC3) && (next & 0xC0) == 0x80)
        {
            out.push_back(static_cast<char>(((c & 0x1Fu) << 6) | (next & 0x3Fu)));
            ++i;
            continue;
        }
        if (c == '"' || c == '\\') { out.push_back('\\'); }
        out.push_back(static_cast<char>(c));
    }
    out.push_back('"');
}

[[nodiscard]] std::string_view sender_of(const CanDbMessage& msg) noexcept
{
    return msg.sender.empty() ? no_node : std::string_view(msg.sender);
}

void append_signal(std::string& out, const CanDbSignal& sig)
{
    out += " SG_ ";
    out += sig.name;
    if (sig.is_muxer) { out += " M"; }
    else if (sig.is_muxed) { std::format_to(std::back_inserter(out), " m{}", sig.mux_value); }
    std::format_to(std::back_inserter(out), " : {}|{}@{}{} (", dbc_start_bit(sig), sig.length,
                   sig.big_endian ? 0 : 1, sig.is_unsigned ? '+' : '-');
    append_number(out, sig.factor);
    out.push_back(',');
    append_number(out, sig.offset);
    out += ") [";
    append_number(out, sig.min);
    out.push_back('|');
    append_number(out, sig.max);
    out += "] ";
    append_quoted(out, sig.unit);
    out.push_back(' ');
    out += no_node;   // receivers are not stored
    out.push_back('\n');
}

} // namespace

void dbc_write(const CanDb& db, std::ostream& out)
{
    std::string text{header};

    std::vector<std::string_view> nodes;   // unique senders, first seen first
    for (const auto& [raw_id, msg] : db.messages)
    {
        const std::string_view sender = sender_of(msg);
        if (sender != no_node && std::ranges::find(nodes, sender) == nodes.end())
        {
            nodes.push_back(sender);
        }
    }
    for (const std::string_view node : nodes)
    {
        text.push_back(' ');
        text += node;
    }
    text += "\n\n";

    for (const auto& [raw_id, msg] : db.messages)
    {
        std::format_to(std::back_inserter(text), "BO_ {} {}: {} {}\n", msg.raw_id, msg.name, msg.dlc, sender_of(msg));
        for (const CanDbSignal& sig : msg.signals)
        {
            append_signal(text, sig);
        }
        text.push_back('\n');
    }

    for (const auto& [raw_id, msg] : db.messages)
    {
        if (!msg.comment.empty())
        {
            std::format_to(std::back_inserter(text), "CM_ BO_ {} ", msg.raw_id);
            append_quoted(text, msg.comment);
            text += ";\n";
        }
        for (const CanDbSignal& sig : msg.signals)
        {
            if (sig.comment.empty()) { continue; }
            std::format_to(std::back_inserter(text), "CM_ SG_ {} {} ", msg.raw_id, sig.name);
            append_quoted(text, sig.comment);
            text += ";\n";
        }
    }

    for (const auto& [raw_id, msg] : db.messages)
    {
        for (const CanDbSignal& sig : msg.signals)
        {
            if (sig.value_table.empty()) { continue; }
            std::format_to(std::back_inserter(text), "VAL_ {} {}", msg.raw_id, sig.name);
            for (const auto& [value, name] : sig.value_table)
            {
                std::format_to(std::back_inserter(text), " {} ", value);
                append_quoted(text, name);
            }
            text += " ;\n";
        }
    }

    for (const auto& [raw_id, msg] : db.messages)
    {
        for (const CanDbSignal& sig : msg.signals)
        {
            if (sig.value_type == SignalValueType::integer) { continue; }
            std::format_to(std::back_inserter(text), "SIG_VALTYPE_ {} {} : {};\n", msg.raw_id, sig.name,
                           sig.value_type == SignalValueType::float32 ? 1 : 2);
        }
    }

    out << text;
}

bool dbc_write_file(const CanDb& db, const std::filesystem::path& path, std::string* error)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        if (error) { *error = std::format("cannot open {} for writing", path.string()); }
        return false;
    }
    dbc_write(db, out);
    out.flush();
    if (!out)
    {
        if (error) { *error = std::format("error writing {}", path.string()); }
        return false;
    }
    return true;
}

unsigned dbc_start_bit(const CanDbSignal& sig) noexcept
{
    if (!sig.big_endian) { return sig.start_bit; }
    return (sig.start_bit / 8u) * 8u + 7u - (sig.start_bit % 8u);
}

void dbc_set_start_bit(CanDbSignal& sig, unsigned dbc_bit) noexcept
{
    sig.start_bit = static_cast<uint16_t>(dbc_bit);
    sig.start_bit = static_cast<uint16_t>(dbc_start_bit(sig)); // the mapping is its own inverse
}
