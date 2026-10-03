#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/text.h"

TEST_CASE("durations read as seconds, minutes, hours and days")
{
    CHECK(format_duration(0.0) == "0.000");
    CHECK(format_duration(12.3456) == "12.346");
    CHECK(format_duration(59.9996) == "1:00.000"); // rounded before splitting
    CHECK(format_duration(245.25) == "4:05.250");
    CHECK(format_duration(11045.25) == "3:04:05.250");
    CHECK(format_duration(1107664.0) == "12d 19:41:04.000");
    CHECK(format_duration(1107664.0, 0) == "12d 19:41:04");
    CHECK(format_duration(-90.5, 1) == "-1:30.5");

    CHECK(parse_duration("90") == 90.0);
    CHECK(parse_duration(" 1:30 ") == 90.0);
    CHECK(parse_duration("1:02:03.5") == 3723.5);
    CHECK(parse_duration("2d 1:02:03") == 2 * 86400.0 + 3723.0);
    CHECK(parse_duration("2d") == 2 * 86400.0);
    CHECK(parse_duration("1d 0:10") == 86400.0 + 600.0); // after days: h:m
    CHECK(parse_duration("1d 5") == 86405.0);
    CHECK(parse_duration(format_duration(1107664.25)) == 1107664.25);
    CHECK_FALSE(parse_duration("").has_value());
    CHECK_FALSE(parse_duration("1:2:3:4").has_value());
    CHECK_FALSE(parse_duration("abc").has_value());
    CHECK_FALSE(parse_duration("-5").has_value());
}

TEST_CASE("big numbers are grouped by thousands")
{
    const auto grouped = [](uint64_t n)
    {
        std::string s;
        append_grouped(s, n);
        return s;
    };
    CHECK(grouped(0) == "0");
    CHECK(grouped(999) == "999");
    CHECK(grouped(1000) == "1 000");
    CHECK(grouped(182764569) == "182 764 569");
    CHECK(grouped(1000000000) == "1 000 000 000");
}
