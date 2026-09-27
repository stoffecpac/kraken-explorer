#pragma once

#include <algorithm>
#include <span>

// Summary statistics of a sample set. Header-only, no allocation.
struct Stats
{
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    double median = 0.0;
};

// Reorders v (std::nth_element) to find the median, so pass a scratch copy you may shuffle.
// Even count: mean of the two middle values. Empty span: all zero.
[[nodiscard]] inline Stats stats_of(std::span<double> v) noexcept
{
    if (v.empty())
    {
        return {};
    }
    Stats s{.min = v[0], .max = v[0]};
    double sum = 0.0;
    for (const double x : v)
    {
        s.min = std::min(s.min, x);
        s.max = std::max(s.max, x);
        sum += x;
    }
    s.mean = sum / static_cast<double>(v.size());
    const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    s.median = *mid;
    if (v.size() % 2 == 0)
    {
        s.median = (s.median + *std::max_element(v.begin(), mid)) / 2.0; // lower middle = max of the left half
    }
    return s;
}
