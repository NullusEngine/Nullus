#include "Assets/AssetDatabaseRetirementScheduler.h"

#include <exception>
#include <memory>
#include <utility>

namespace NLS::Editor::Assets
{
std::future<void> ScheduleAssetDatabaseRetirementWorker(
    std::mutex& mutex,
    bool& workerRunning,
    std::function<void(std::function<void()>)> schedule,
    std::function<void()> worker)
{
    {
        std::lock_guard lock(mutex);
        if (workerRunning)
            return {};
        workerRunning = true;
    }

    auto completion = std::make_shared<std::promise<void>>();
    auto future = completion->get_future();
    try
    {
        schedule([&mutex, &workerRunning, completion, worker = std::move(worker)]() mutable
        {
            try
            {
                worker();
                completion->set_value();
            }
            catch (...)
            {
                {
                    std::lock_guard lock(mutex);
                    workerRunning = false;
                }
                completion->set_exception(std::current_exception());
            }
        });
    }
    catch (...)
    {
        {
            std::lock_guard lock(mutex);
            workerRunning = false;
        }
        completion->set_exception(std::current_exception());
    }
    return future;
}
}
