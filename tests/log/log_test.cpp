// core/log ring buffer: order, wrap-around at capacity, clear.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <format>
#include <string>

#include "core/log.h"

TEST_CASE("log ring keeps the newest entries in order")
{
    log_clear();
    auto& s = log_state();
    const auto total_before = s.total;

    const size_t n = LogState::capacity + 7;
    for (size_t i = 0; i < n; ++i)
    {
        log_info(std::format("msg {}", i));
    }

    std::scoped_lock lock(s.mutex);
    CHECK(s.total - total_before == n);
    REQUIRE(s.entries.size() == LogState::capacity);
    CHECK(log_entry_at(s, 0).text == "msg 7");
    CHECK(log_entry_at(s, LogState::capacity - 1).text == std::format("msg {}", n - 1));
    CHECK(log_entry_at(s, 0).level == LogLevel::Info);
}

TEST_CASE("log_clear empties the ring")
{
    log_warning("x");
    log_clear();
    CHECK(log_state().entries.empty());
    log_error("y");
    std::scoped_lock lock(log_state().mutex);
    CHECK(log_entry_at(log_state(), 0).text == "y");
    CHECK(log_level_name(log_entry_at(log_state(), 0).level) == "error");
}
