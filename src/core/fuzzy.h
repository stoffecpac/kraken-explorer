#pragma once

// fzf-style fuzzy matching (the fzf "v1" algorithm): case-insensitive subsequence match,
// scored on the shortest window found by a forward then a backward scan. No allocation
// except the optional positions vector, which is cleared and reused.

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/text.h"

namespace fuzzy_detail
{

enum class CharClass
{
    Separator, // '.', '_', ' ', '-', '/' and anything that isn't alphanumeric
    Lower,
    Upper,
    Digit,
};

constexpr int score_match = 16;
constexpr int gap_start = -3;
constexpr int gap_extension = -1;
constexpr int bonus_boundary = 8;    // first char, or after a separator
constexpr int bonus_camel = 7;       // lower -> Upper, letter -> digit
constexpr int bonus_consecutive = 4; // = -(gap_start + gap_extension): a run never loses to a gap
constexpr int first_char_multiplier = 2;

constexpr CharClass classify(char c) noexcept
{
    if (c >= 'a' && c <= 'z')
    {
        return CharClass::Lower;
    }
    if (c >= 'A' && c <= 'Z')
    {
        return CharClass::Upper;
    }
    if (c >= '0' && c <= '9')
    {
        return CharClass::Digit;
    }
    return CharClass::Separator;
}

constexpr int bonus_for(CharClass prev, CharClass cur) noexcept
{
    if (cur == CharClass::Separator)
    {
        return 0;
    }
    if (prev == CharClass::Separator)
    {
        return bonus_boundary;
    }
    if ((prev == CharClass::Lower && cur == CharClass::Upper) || (prev != CharClass::Digit && cur == CharClass::Digit))
    {
        return bonus_camel;
    }
    return 0;
}

} // namespace fuzzy_detail

// -1 = pattern is not a subsequence of text; otherwise higher is better. An empty pattern
// matches everything with score 0. positions (optional) receives the matched text indices.
[[nodiscard]] inline int fuzzy_score(std::string_view pattern, std::string_view text, std::vector<int>* positions = nullptr)
{
    using namespace fuzzy_detail;
    if (positions != nullptr)
    {
        positions->clear();
    }
    if (pattern.empty())
    {
        return 0;
    }
    // Forward: the earliest end of a subsequence match.
    std::size_t pi = 0;
    std::size_t end = 0;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (ascii_lower(text[i]) == ascii_lower(pattern[pi]) && ++pi == pattern.size())
        {
            end = i + 1;
            break;
        }
    }
    if (pi < pattern.size())
    {
        return -1;
    }
    // Backward from there: the latest start, i.e. the shortest window ending at `end`.
    std::size_t begin = end;
    for (std::size_t i = end; i-- > 0;)
    {
        if (ascii_lower(text[i]) == ascii_lower(pattern[pi - 1]) && --pi == 0)
        {
            begin = i;
            break;
        }
    }
    int score = 0;
    int consecutive = 0;
    int first_bonus = 0;
    bool in_gap = false;
    CharClass prev = begin == 0 ? CharClass::Separator : classify(text[begin - 1]);
    for (std::size_t i = begin; i < end; ++i)
    {
        const CharClass cls = classify(text[i]);
        if (pi < pattern.size() && ascii_lower(text[i]) == ascii_lower(pattern[pi]))
        {
            int bonus = bonus_for(prev, cls);
            if (consecutive == 0)
            {
                first_bonus = bonus;
            }
            else
            {
                // A run keeps the bonus of its start (a boundary inside the run takes over).
                if (bonus >= bonus_boundary && bonus > first_bonus)
                {
                    first_bonus = bonus;
                }
                bonus = std::max({bonus, first_bonus, bonus_consecutive});
            }
            score += score_match + (pi == 0 ? bonus * first_char_multiplier : bonus);
            in_gap = false;
            ++consecutive;
            ++pi;
            if (positions != nullptr)
            {
                positions->push_back(static_cast<int>(i));
            }
        }
        else
        {
            score += in_gap ? gap_extension : gap_start;
            in_gap = true;
            consecutive = 0;
            first_bonus = 0;
        }
        prev = cls;
    }
    return score;
}

// ripgrep's --smart-case substring search: case-insensitive unless the pattern has an upper-case
// letter. Index of the first match, npos when none; an empty pattern matches at 0.
[[nodiscard]] inline std::size_t smart_find(std::string_view text, std::string_view pattern) noexcept
{
    if (std::ranges::any_of(pattern, [](char c) { return c >= 'A' && c <= 'Z'; }))
    {
        return text.find(pattern);
    }
    const auto hit = std::ranges::search(text, pattern, {}, ascii_lower, ascii_lower);
    return hit.empty() && !pattern.empty() ? std::string_view::npos : static_cast<std::size_t>(hit.begin() - text.begin());
}
