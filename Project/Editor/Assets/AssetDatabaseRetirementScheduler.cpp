#include "Assets/AssetDatabaseRetirementScheduler.h"

#include <atomic>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

namespace NLS::Editor::Assets
{
namespace
{
class RetirementTerminalState
{
public:
    RetirementTerminalState(
        std::mutex& mutex,
        bool& workerRunning,
        std::function<bool()> hasPendingWork)
        : m_mutex(mutex),
          m_workerRunning(workerRunning),
          m_hasPendingWork(std::move(hasPendingWork))
    {
    }

    std::future<void> GetFuture()
    {
        return m_completion.get_future();
    }

    bool CompleteSuccessOrContinue()
    {
        std::unique_lock lock(m_mutex);
        if (m_completed.load(std::memory_order_acquire))
            return false;
        if (m_hasPendingWork && m_hasPendingWork())
            return true;
        if (m_completed.exchange(true, std::memory_order_acq_rel))
            return false;

        m_workerRunning = false;
        lock.unlock();
        m_completion.set_value();
        return false;
    }

    void CompleteFailure(std::exception_ptr failure)
    {
        Complete(std::move(failure));
    }

private:
    void Complete(std::exception_ptr failure)
    {
        if (m_completed.exchange(true, std::memory_order_acq_rel))
            return;

        {
            std::lock_guard lock(m_mutex);
            m_workerRunning = false;
        }
        if (failure)
            m_completion.set_exception(std::move(failure));
        else
            m_completion.set_value();
    }

    std::mutex& m_mutex;
    bool& m_workerRunning;
    std::function<bool()> m_hasPendingWork;
    std::atomic_bool m_completed = false;
    std::promise<void> m_completion;
};
}

std::future<void> ScheduleAssetDatabaseRetirementWorker(
    std::mutex& mutex,
    bool& workerRunning,
    AssetDatabaseRetirementSchedule schedule,
    std::function<void()> worker,
    std::function<bool()> hasPendingWork)
{
    {
        std::lock_guard lock(mutex);
        if (workerRunning)
            return {};
        workerRunning = true;
    }

    auto terminal = std::make_shared<RetirementTerminalState>(
        mutex,
        workerRunning,
        std::move(hasPendingWork));
    auto future = terminal->GetFuture();
    try
    {
        const bool accepted = schedule(
            [terminal, worker = std::move(worker)]() mutable
            {
                try
                {
                    do
                    {
                        worker();
                    }
                    while (terminal->CompleteSuccessOrContinue());
                }
                catch (...)
                {
                    terminal->CompleteFailure(std::current_exception());
                }
            },
            [terminal]
            {
                terminal->CompleteFailure(std::make_exception_ptr(
                    std::runtime_error("asset database retirement job cancelled before execution")));
            });
        if (!accepted)
        {
            terminal->CompleteFailure(std::make_exception_ptr(
                std::runtime_error("asset database retirement job scheduling rejected")));
        }
    }
    catch (...)
    {
        terminal->CompleteFailure(std::current_exception());
    }
    return future;
}
}
