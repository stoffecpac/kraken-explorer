#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <vector>

#include "core/stats.h"

// Hand-computed values.
TEST_CASE("odd count: middle element")
{
    std::vector<double> v{9.0, 1.0, 5.0, 3.0, 7.0};
    const Stats s = stats_of(v);
    CHECK(s.min == 1.0);
    CHECK(s.max == 9.0);
    CHECK(s.mean == doctest::Approx(5.0));
    CHECK(s.median == 5.0);
}

TEST_CASE("even count: mean of the two middle elements")
{
    std::vector<double> v{10.0, 2.0, 4.0, 100.0};
    const Stats s = stats_of(v);
    CHECK(s.median == doctest::Approx(7.0));
    CHECK(s.mean == doctest::Approx(29.0));
    std::vector<double> two{1.0, 2.0};
    CHECK(stats_of(two).median == doctest::Approx(1.5));
    std::vector<double> one{4.5};
    CHECK(stats_of(one).median == 4.5);
    CHECK(stats_of({}).median == 0.0);
}
