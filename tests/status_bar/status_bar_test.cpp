#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>

#include "ui/status_bar.h"

using namespace std::chrono_literals;

// 50 frames/s from cangen, counted as Trace::end; values worked out by hand.
TEST_CASE("frame rate covers only the measurement's own frames")
{
    StatusBarState s;
    const auto t0 = std::chrono::steady_clock::time_point{} + 100s;

    // Idle with frames already in the trace (earlier measurement, replay): no rate, no base yet.
    status_bar_sample(s, false, 48713, t0 - 5s);
    CHECK(s.rate == 0.0);

    // Start: the first measuring sample re-bases instead of counting the 48713 earlier frames.
    status_bar_sample(s, true, 48713, t0);
    CHECK(s.rate == 0.0);
    status_bar_sample(s, true, 48738, t0 + 500ms); // less than a second: keep the last rate
    CHECK(s.rate == 0.0);
    status_bar_sample(s, true, 48763, t0 + 1s);
    CHECK(s.rate == doctest::Approx(50.0)); // 50 frames in 1 s
    status_bar_sample(s, true, 48838, t0 + 2500ms);
    CHECK(s.rate == doctest::Approx(50.0)); // 75 frames in 1.5 s

    // Stop: 0 at once, not the last rate for up to a second.
    status_bar_sample(s, false, 48850, t0 + 2700ms);
    CHECK(s.rate == 0.0);

    // Frames appended while idle (replay) do not count towards the next measurement.
    status_bar_sample(s, false, 60000, t0 + 9s);
    status_bar_sample(s, true, 60000, t0 + 10s);
    status_bar_sample(s, true, 60100, t0 + 12s);
    CHECK(s.rate == doctest::Approx(50.0)); // 100 frames in 2 s
}
