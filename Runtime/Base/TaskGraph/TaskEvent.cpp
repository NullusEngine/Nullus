#include "TaskGraph/TaskEvent.h"

namespace NLS::Base::TaskGraph
{
    uint64_t TaskEvent::Trigger() noexcept
    {
        std::lock_guard lock(m_mutex);
        ++m_generation;
        m_triggered = true;
        m_condition.notify_all();
        return m_generation;
    }

    void TaskEvent::Reset() noexcept
    {
        std::lock_guard lock(m_mutex);
        m_triggered = false;
    }

    uint64_t TaskEvent::Generation() const noexcept
    {
        std::lock_guard lock(m_mutex);
        return m_generation;
    }

    bool TaskEvent::IsTriggered() const noexcept
    {
        std::lock_guard lock(m_mutex);
        return m_triggered;
    }

    void TaskEvent::Wait()
    {
        std::unique_lock lock(m_mutex);
        m_condition.wait(lock, [this] { return m_triggered; });
    }

    bool TaskEvent::WaitFor(const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(m_mutex);
        return m_condition.wait_for(lock, timeout, [this] { return m_triggered; });
    }

    bool TaskEvent::WaitForGeneration(
        const uint64_t generation,
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(m_mutex);
        return m_condition.wait_for(
            lock,
            timeout,
            [this, generation]
            {
                return m_generation != generation;
            });
    }
}
