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

// Small text helpers shared by the trace, DBC and replay parsers.

#include <algorithm>
#include <charconv>
#include <string_view>
#include <type_traits>

// Whole-string conversion: fails on empty input or trailing characters.
template <typename T>
[[nodiscard]] bool parse_number(std::string_view s, T& value, int base = 10)
{
    const char* end = s.data() + s.size();
    std::from_chars_result r{};
    if constexpr (std::is_floating_point_v<T>)
    {
        r = std::from_chars(s.data(), end, value);
    }
    else
    {
        r = std::from_chars(s.data(), end, value, base);
    }
    return r.ec == std::errc{} && r.ptr == end;
}

[[nodiscard]] constexpr char ascii_lower(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// ASCII case-insensitive equality.
[[nodiscard]] constexpr bool iequals(std::string_view a, std::string_view b) noexcept
{
    return std::ranges::equal(a, b, {}, ascii_lower, ascii_lower);
}

// Strips leading and trailing whitespace.
[[nodiscard]] constexpr std::string_view trim(std::string_view s) noexcept
{
    constexpr std::string_view space = " \t\n\r\f\v";
    const auto first = s.find_first_not_of(space);
    return first == std::string_view::npos ? std::string_view{} : s.substr(first, s.find_last_not_of(space) - first + 1);
}
