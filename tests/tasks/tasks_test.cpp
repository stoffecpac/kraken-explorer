// core/tasks: posted work runs on the draining thread, blocking calls return and rethrow.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "core/tasks.h"

struct App
{
    std::vector<int> seen;
};

TEST_CASE("posted tasks run in order on drain, move-only captures allowed")
{
    Tasks tasks;
    App app;
    int wakes = 0;
    static int* wake_counter = nullptr;
    wake_counter = &wakes;
    tasks.wake = [] { ++*wake_counter; };

    auto p = std::make_unique<int>(2);
    tasks_post(tasks, [](App& a) { a.seen.push_back(1); });
    tasks_post(tasks, [p = std::move(p)](App& a) { a.seen.push_back(*p); });
    CHECK(app.seen.empty());
    CHECK(wakes == 2);

    tasks_drain(tasks, app);
    CHECK(app.seen == std::vector<int>{1, 2});
    tasks_drain(tasks, app);
    CHECK(app.seen.size() == 2);
}

TEST_CASE("task posted while draining runs on the next drain")
{
    Tasks tasks;
    App app;
    tasks_post(tasks, [&tasks](App& a)
    {
        a.seen.push_back(1);
        tasks_post(tasks, [](App& b) { b.seen.push_back(2); });
    });
    tasks_drain(tasks, app);
    CHECK(app.seen == std::vector<int>{1});
    tasks_drain(tasks, app);
    CHECK(app.seen == std::vector<int>{1, 2});
}

TEST_CASE("run_on_main_blocking from a worker runs on the main thread and rethrows")
{
    Tasks tasks;
    App app;
    const auto main_id = std::this_thread::get_id();
    std::atomic<bool> finished = false;
    std::thread::id ran_on;
    bool threw = false;

    std::jthread worker([&]
    {
        tasks_run_on_main_blocking(tasks, app, [&](App& a)
        {
            ran_on = std::this_thread::get_id();
            a.seen.push_back(7);
        });
        try
        {
            tasks_run_on_main_blocking(tasks, app, [](App&) { throw std::runtime_error("boom"); });
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        finished = true;
    });

    while (!finished)
    {
        tasks_drain(tasks, app);
        std::this_thread::yield();
    }
    worker.join();
    CHECK(ran_on == main_id);
    CHECK(app.seen == std::vector<int>{7});
    CHECK(threw);
}

TEST_CASE("run_on_main_blocking on the main thread runs inline")
{
    Tasks tasks;
    App app;
    tasks_run_on_main_blocking(tasks, app, [](App& a) { a.seen.push_back(3); });
    CHECK(app.seen == std::vector<int>{3});
    CHECK(tasks.queue.empty());
}
