#include "sym_parser.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/text.h"
#include "db/dbf/dbf.h"
#include "db/dbc/dbc_parser.h"

namespace
{

using Enums = std::map<std::string, std::map<uint64_t, std::string>, std::less<>>;

// Valid UTF-8 stays as is; anything else is taken as Latin-1 (PCAN writes ANSI).
std::string to_utf8(std::string_view text)
{
    std::size_t i = 0;
    while (i < text.size())
    {
        const auto c = static_cast<unsigned char>(text[i]);
        const std::size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (n == 0 || i + n > text.size()) { break; }
        bool ok = true;
        for (std::size_t k = 1; k < n; ++k) { ok = ok && (static_cast<unsigned char>(text[i + k]) >> 6) == 0x2; }
        if (!ok) { break; }
        i += n;
    }
    if (i == text.size()) { return std::string(text); }
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    for (const char ch : text)
    {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x80) { out.push_back(ch); continue; }
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
    return out;
}

// Position of "//" outside quotes, npos if none.
std::size_t comment_pos(std::string_view line)
{
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i)
    {
        if (line[i] == '"') { quoted = !quoted; }
        else if (!quoted && line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/') { return i; }
    }
    return std::string_view::npos;
}

// Splits on whitespace and ',' outside quotes; quotes stay in the token.
std::vector<std::string_view> split(std::string_view s)
{
    std::vector<std::string_view> out;
    bool quoted = false;
    std::size_t start = std::string_view::npos;
    for (std::size_t i = 0; i <= s.size(); ++i)
    {
        const bool end = i == s.size();
        const char c = end ? ' ' : s[i];
        if (c == '"') { quoted = !quoted; }
        const bool sep = end || (!quoted && (c == ' ' || c == '\t' || c == ','));
        if (sep && start != std::string_view::npos) { out.push_back(s.substr(start, i - start)); start = std::string_view::npos; }
        else if (!sep && start == std::string_view::npos) { start = i; }
    }
    return out;
}

std::string_view unquote(std::string_view s)
{
    return s.size() >= 2 && s.front() == '"' && s.back() == '"' ? s.substr(1, s.size() - 2) : s;
}

// Decimal, or hex with a trailing 'h' ("1Fh").
template <typename T>
bool parse_int(std::string_view s, T& v)
{
    if (!s.empty() && (s.back() == 'h' || s.back() == 'H')) { return parse_number(s.substr(0, s.size() - 1), v, 16); }
    return parse_number(s, v);
}

struct Line
{
    std::string key;
    std::string value;
    std::string comment;
};

struct Block
{
    std::string name;
    std::vector<Line> lines;
};

struct Ctx
{
    CanDb& db;
    Enums enums;
    std::map<std::string, CanDbSignal, std::less<>> signal_defs;   // {SIGNALS}
    std::map<std::string, uint32_t, std::less<>> by_name;          // symbol name -> raw id (later mux blocks)
};

// Type + optional explicit length; false for an unknown type that is no enum.
bool apply_type(Ctx& ctx, CanDbSignal& sig, std::string_view type, std::optional<uint16_t> length)
{
    sig.is_unsigned = type != "signed";
    if (type == "bit") { sig.length = 1; sig.max = 1; }
    else if (type == "char") { sig.length = 8; }
    else if (type == "float") { sig.length = 32; sig.value_type = SignalValueType::float32; }
    else if (type == "double") { sig.length = 64; sig.value_type = SignalValueType::float64; }
    else
    {
        sig.length = length.value_or(0);
        if (type != "signed" && type != "unsigned" && type != "string" && type != "raw")
        {
            const auto e = ctx.enums.find(type);
            if (e == ctx.enums.end()) { return false; }
            sig.value_table = e->second;
        }
    }
    return true;
}

// Flags and /x: attributes from tokens[i..].
void apply_attrs(Ctx& ctx, CanDbSignal& sig, std::span<const std::string_view> tokens)
{
    for (const std::string_view t : tokens)
    {
        auto num = [&](std::string_view prefix, double& v)
        {
            if (t.starts_with(prefix) && !parse_number(t.substr(prefix.size()), v))
            {
                log_warning(std::format("sym: bad number in '{}' of signal {}", t, sig.name));
            }
        };
        if (t == "-m") { sig.big_endian = true; }
        else if (t.starts_with("/u:")) { sig.unit = unquote(t.substr(3)); }
        else if (t.starts_with("/e:"))
        {
            const auto e = ctx.enums.find(t.substr(3));
            if (e != ctx.enums.end()) { sig.value_table = e->second; }
            else { log_warning(std::format("sym: enum {} of signal {} is not defined", t.substr(3), sig.name)); }
        }
        else if (t.starts_with("/ln:") && sig.comment.empty()) { sig.comment = unquote(t.substr(4)); }
        else
        {
            num("/f:", sig.factor);
            num("/o:", sig.offset);
            num("/min:", sig.min);
            num("/max:", sig.max);
        }
        // -h -b -s -v -t -p, /d: /p: /spn: carry display/default data the model has no place for.
    }
}

// "Name type start,length flags attrs" (Var) or "Name start" (Sig reference).
bool parse_signal(Ctx& ctx, const Line& l, CanDbSignal& sig)
{
    const auto t = split(l.value);
    if (l.key == "Sig")
    {
        uint16_t start = 0;
        const auto def = t.size() >= 2 ? ctx.signal_defs.find(t[0]) : ctx.signal_defs.end();
        if (def == ctx.signal_defs.end() || !parse_number(t[1], start)) { return false; }
        sig = def->second;
        sig.start_bit = start;
        return true;
    }
    uint16_t length = 0;
    if (t.size() < 4 || !parse_number(t[2], sig.start_bit) || !parse_number(t[3], length)) { return false; }
    sig.name = t[0];
    sig.comment = l.comment;
    if (!apply_type(ctx, sig, t[1], length)) { return false; }
    apply_attrs(ctx, sig, std::span(t).subspan(4));
    // Motorola: .sym numbers the MSB like cantools' sym.py, which flips the bit within its
    // byte to reach the DBC start bit; CanDb's sequential index flips it back, so it is 1:1.
    return true;
}

void flush_block(Ctx& ctx, const Block& b)
{
    if (b.name.empty()) { return; }
    auto find = [&](std::string_view key) -> const Line*
    {
        for (const Line& l : b.lines) { if (l.key == key) { return &l; } }
        return nullptr;
    };

    CanDbMessage* msg = nullptr;
    if (const Line* id = find("ID"))
    {
        // ponytail: an id range "100h-10Fh" maps to its first id only; expand when someone ships one.
        const auto tokens = split(id->value);
        const std::string_view first = tokens.empty() ? std::string_view{} : tokens[0].substr(0, tokens[0].find('-', 1));
        uint32_t raw = 0;
        if (!parse_int(first, raw))
        {
            log_warning(std::format("sym: bad ID '{}' in [{}]", id->value, b.name));
            return;
        }
        const Line* type = find("Type");
        const bool extended = first.size() == 9 || (type && (iequals(trim(type->value), "Extended") || iequals(trim(type->value), "FDExtended")));
        if (extended) { raw |= 0x80000000u; }
        uint32_t len = 8;
        if (const Line* l = find("Len")) { (void)parse_number(trim(l->value), len); }
        msg = &ctx.db.messages[raw];
        msg->name = b.name;
        msg->raw_id = raw;
        msg->dlc = static_cast<uint8_t>(len);
        msg->comment = id->comment;
        ctx.by_name[b.name] = raw;
    }
    else if (const auto it = ctx.by_name.find(b.name); it != ctx.by_name.end())
    {
        msg = &ctx.db.messages[it->second];
    }
    else
    {
        log_warning(std::format("sym: [{}] has no ID", b.name));
        return;
    }

    std::optional<uint32_t> mux_value;
    if (const Line* mux = find("Mux"))
    {
        // "Name start,length value [-m] [-t]"
        const auto t = split(mux->value);
        CanDbSignal m{.name = t.empty() ? std::string{} : std::string(t[0]), .is_unsigned = true, .is_muxer = true, .comment = mux->comment};
        uint32_t value = 0;
        if (t.size() < 4 || !parse_number(t[1], m.start_bit) || !parse_number(t[2], m.length) || !parse_int(t[3], value))
        {
            log_warning(std::format("sym: bad Mux '{}' in [{}]", mux->value, b.name));
            return;
        }
        m.big_endian = std::ranges::find(t, std::string_view("-m")) != t.end();
        if (msg->muxer < 0)
        {
            msg->signals.push_back(std::move(m));
            msg->muxer = static_cast<int>(msg->signals.size() - 1);
        }
        mux_value = value;
    }

    for (const Line& l : b.lines)
    {
        if (l.key != "Var" && l.key != "Sig") { continue; }
        CanDbSignal sig;
        if (!parse_signal(ctx, l, sig))
        {
            log_warning(std::format("sym: skipped {}={} in [{}]", l.key, l.value, b.name));
            continue;
        }
        if (mux_value)
        {
            sig.is_muxed = true;
            sig.mux_value = *mux_value;
        }
        msg->signals.push_back(std::move(sig));
    }
}

// "Name(0="a", 1="b")", parentheses already balanced.
void parse_enum(Ctx& ctx, std::string_view v)
{
    const auto open = v.find('(');
    const auto close = v.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) { return; }
    auto& table = ctx.enums[std::string(trim(v.substr(0, open)))];
    for (const std::string_view item : split(v.substr(open + 1, close - open - 1)))
    {
        const auto eq = item.find('=');
        int64_t value = 0;
        if (eq == std::string_view::npos || !parse_int(item.substr(0, eq), value)) { continue; }
        table[static_cast<uint64_t>(value)] = unquote(item.substr(eq + 1));
    }
}

// {SIGNALS}: "Name type [length] [-m] attrs".
void parse_signal_def(Ctx& ctx, std::string_view v)
{
    const auto t = split(v);
    if (t.size() < 2) { return; }
    CanDbSignal sig{.name = std::string(t[0])};
    std::optional<uint16_t> length;
    std::size_t next = 2;
    if (uint16_t n = 0; t.size() > 2 && parse_number(t[2], n)) { length = n; next = 3; }
    if (!apply_type(ctx, sig, t[1], length))
    {
        log_warning(std::format("sym: unknown type {} of signal {}", t[1], sig.name));
        return;
    }
    apply_attrs(ctx, sig, std::span(t).subspan(next));
    ctx.signal_defs[sig.name] = std::move(sig);
}

} // namespace

bool sym_parse(std::string_view input, CanDb& db)
{
    const std::string text = to_utf8(input);
    if (!text.contains("FormatVersion"))
    {
        log_error("sym: no FormatVersion, not a PCAN symbol file");
        return false;
    }
    Ctx ctx{.db = db};
    std::string section;
    Block block;
    std::string pending_enum;   // an Enum= that spans lines

    std::size_t pos = 0;
    while (pos < text.size())
    {
        const auto nl = text.find('\n', pos);
        std::string_view raw = std::string_view(text).substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;

        const auto cpos = comment_pos(raw);
        const std::string_view comment = cpos == std::string_view::npos ? std::string_view{} : trim(raw.substr(cpos + 2));
        const std::string_view line = trim(raw.substr(0, cpos));
        if (line.empty()) { continue; }

        if (!pending_enum.empty() || (section == "{ENUMS}" && line.starts_with("Enum=")))
        {
            pending_enum.append(line).push_back(' ');
            if (std::ranges::count(pending_enum, '(') <= std::ranges::count(pending_enum, ')'))
            {
                parse_enum(ctx, std::string_view(pending_enum).substr(pending_enum.find('=') + 1));
                pending_enum.clear();
            }
            continue;
        }
        if (line.front() == '{')
        {
            flush_block(ctx, block);
            block = {};
            section = line;
            continue;
        }
        if (line.front() == '[' && line.back() == ']')
        {
            flush_block(ctx, block);
            block = {.name = std::string(line.substr(1, line.size() - 2))};
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) { continue; }
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        // Header (FormatVersion, Title): file metadata, nothing reads it.
        if (section == "{SIGNALS}" && key == "Sig") { parse_signal_def(ctx, value); }
        else if ((section == "{SEND}" || section == "{RECEIVE}" || section == "{SENDRECEIVE}") && !block.name.empty())
        {
            block.lines.push_back({std::string(key), std::string(value), std::string(comment)});
        }
        // {VIRTUALVARS} and unknown sections: skipped.
    }
    flush_block(ctx, block);
    return true;
}

bool sym_parse_file(const std::filesystem::path& path, CanDb& db)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        log_error(std::format("error parsing sym file {}: cannot open", path.string()));
        return false;
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (!sym_parse(text, db)) { return false; }
    db.path = path.string();
    return true;
}

bool can_db_parse_file(const std::filesystem::path& path, CanDb& db)
{
    const std::string ext = path.extension().string();
    if (iequals(ext, ".dbf"))
    {
        std::string error;
        if (!dbf_parse_file(path, db, &error))
        {
            log_error(std::format("{}: {}", path.string(), error));
            return false;
        }
        return true;
    }
    return iequals(ext, ".sym") ? sym_parse_file(path, db) : dbc_parse_file(path, db);
}
