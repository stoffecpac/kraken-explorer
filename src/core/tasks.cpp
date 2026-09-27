#include "core/tasks.h"

#include <exception>
#include <future>
#include <utility>

void tasks_post(Tasks& tasks, Task fn)
{
    {
        std::scoped_lock lock(tasks.mutex);
        tasks.queue.push_back(std::move(fn));
    }
    if (tasks.wake)
    {
        tasks.wake();
    }
}

void tasks_run_on_main_blocking(Tasks& tasks, App& app, Task fn)
{
    if (std::this_thread::get_id() == tasks.main_thread)
    {
        fn(app);
        return;
    }
    std::promise<void> done;
    auto result = done.get_future();
    tasks_post(tasks, [&fn, done = std::move(done)](App& a) mutable
    {
        try
        {
            fn(a);
            done.set_value();
        }
        catch (...)
        {
            done.set_exception(std::current_exception());
        }
    });
    result.get();
}

void tasks_drain(Tasks& tasks, App& app)
{
    std::vector<Task> batch;
    {
        std::scoped_lock lock(tasks.mutex);
        batch.swap(tasks.queue);
    }
    for (auto& fn : batch)
    {
        fn(app);
    }
}
