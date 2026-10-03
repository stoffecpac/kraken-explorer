#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "ui/log_window.h"

namespace
{

void update(LogWindowState& s)
{
    LogState& log = log_state();
    const std::lock_guard lock(log.mutex);
    log_window_update(s, log);
}

// Texts of the shown lines, oldest first.
std::vector<std::string> shown(const LogWindowState& s)
{
    LogState& log = log_state();
    const std::lock_guard lock(log.mutex);
    std::vector<std::string> out;
    for (const uint64_t a : s.rows)
    {
        out.push_back(log_entry_at(log, a - s.base).text);
    }
    return out;
}

void fill()
{
    log_clear();
    log_debug("probe tick");
    log_info("opened vcan0");
    log_info("measurement started");
    log_warning("bus load 90%");
    log_error("cannot open can1");
    log_error("driver crashed");
}

} // namespace

TEST_CASE("level counts and toggles")
{
    fill();
    LogWindowState s;
    update(s);
    CHECK(s.counts == std::array<size_t, 4>{1, 2, 1, 2});
    CHECK(s.rows.size() == 6);

    s.show[static_cast<size_t>(LogFilterLevel::Info)] = false;
    update(s);
    CHECK(s.rows.size() == 4);
    s.show[static_cast<size_t>(LogFilterLevel::Debug)] = false;
    s.show[static_cast<size_t>(LogFilterLevel::Warning)] = false;
    update(s);
    CHECK(shown(s) == std::vector<std::string>{"cannot open can1", "driver crashed"});
    CHECK(s.counts == std::array<size_t, 4>{1, 2, 1, 2}); // counts ignore the filter

    s.show = {true, true, true, true};
    update(s);
    CHECK(s.rows.size() == 6);
}

TEST_CASE("query: ripgrep smart-case substring")
{
    fill();
    LogWindowState s;
    s.query = "open"; // lower case: any case
    update(s);
    CHECK(shown(s) == std::vector<std::string>{"opened vcan0", "cannot open can1"});
    s.query = "opcan"; // a substring, not a subsequence
    update(s);
    CHECK(s.rows.empty());
    s.query = "Measurement"; // an upper-case letter: exact case
    update(s);
    CHECK(s.rows.empty());
    s.query = "started";
    update(s);
    CHECK(shown(s) == std::vector<std::string>{"measurement started"});
    s.query = "zzz";
    update(s);
    CHECK(s.rows.empty());
}

TEST_CASE("incremental filtering matches only new lines")
{
    fill();
    LogWindowState s;
    s.query = "open";
    update(s);
    REQUIRE(shown(s).size() == 2);
    {
        // Change an old line behind the window's back: an incremental update must not re-match it.
        LogState& log = log_state();
        const std::lock_guard lock(log.mutex);
        log.entries[log.head + 1].text = "renamed";
    }
    log_info("open again");
    log_info("unrelated");
    update(s);
    CHECK(s.rows.size() == 3); // old match kept, one new match appended
    CHECK(shown(s).back() == "open again");
    CHECK(s.counts[static_cast<size_t>(LogFilterLevel::Info)] == 4);

    s.query = "ope"; // query change: full refilter now drops the renamed line
    update(s);
    CHECK(shown(s) == std::vector<std::string>{"cannot open can1", "open again"});

    log_clear();
    update(s);
    CHECK(s.rows.empty());
    CHECK(s.counts == std::array<size_t, 4>{});
}

TEST_CASE("ring wrap evicts rows and counts")
{
    log_clear();
    LogWindowState s;
    log_error("first error");
    update(s);
    for (size_t i = 0; i < LogState::capacity; ++i)
    {
        log_info("x");
    }
    update(s);
    CHECK(s.counts == std::array<size_t, 4>{0, LogState::capacity, 0, 0});
    CHECK(s.rows.size() == LogState::capacity);
    log_warning("w");
    update(s);
    CHECK(s.counts == std::array<size_t, 4>{0, LogState::capacity - 1, 1, 0});
    CHECK(s.rows.size() == LogState::capacity);
}
