#pragma once

#include <functional>
#include <future>
#include <mutex>

namespace NLS::Editor::Assets
{
using AssetDatabaseRetirementSchedule = std::function<bool(
    std::function<void()> run,
    std::function<void()> cancel)>;

// Every accepted, rejected, failed, or cancelled attempt completes the returned
// future and releases workerRunning so a later retirement can be scheduled.
// hasPendingWork is called with mutex held and must not lock it recursively.
std::future<void> ScheduleAssetDatabaseRetirementWorker(
    std::mutex& mutex,
    bool& workerRunning,
    AssetDatabaseRetirementSchedule schedule,
    std::function<void()> worker,
    std::function<bool()> hasPendingWork = {});
}
