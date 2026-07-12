#pragma once

#include <functional>
#include <future>
#include <mutex>

namespace NLS::Editor::Assets
{
// Returns an exceptional completion future when scheduling is rejected. The
// worker owns the normal workerRunning=false transition while draining state.
std::future<void> ScheduleAssetDatabaseRetirementWorker(
    std::mutex& mutex,
    bool& workerRunning,
    std::function<void(std::function<void()>)> schedule,
    std::function<void()> worker);
}
