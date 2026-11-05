#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace NLS::Base::Jobs::Internal
{
    // Park idle workers in LIFO order so a serial stream of small tasks can
    // reuse the most recently active worker. This selects a sleeper only;
    // task priority, queue order and stealing remain with JobQueue.
    class WorkerWakeSignal final
    {
    public:
        struct Waiter final
        {
            std::condition_variable condition;
            Waiter* previous = nullptr;
            Waiter* next = nullptr;
            bool linked = false;
            bool signaled = false;
        };

        void notify_one()
        {
            std::lock_guard lock(m_mutex);
            if (m_last != nullptr)
                SignalLocked(*m_last);
        }

        void notify_all()
        {
            std::lock_guard lock(m_mutex);
            while (m_last != nullptr)
                SignalLocked(*m_last);
        }

#if defined(NLS_ENABLE_TEST_HOOKS)
        size_t GetWaitingCountForTesting()
        {
            std::lock_guard lock(m_mutex);
            size_t count = 0u;
            for (auto* waiter = m_last; waiter; waiter = waiter->previous)
                ++count;
            return count;
        }
#endif

        template<class Predicate>
        void wait(std::unique_lock<std::mutex>& stateLock, Waiter& waiter, Predicate predicate)
        {
            WaitUntil(stateLock, waiter, std::chrono::steady_clock::time_point::max(), predicate);
        }

        template<class Rep, class Period, class Predicate>
        bool wait_for(std::unique_lock<std::mutex>& stateLock, Waiter& waiter,
            std::chrono::duration<Rep, Period> duration, Predicate predicate)
        {
            return WaitUntil(stateLock, waiter, std::chrono::steady_clock::now() + duration, predicate);
        }

    private:
        void UnlinkLocked(Waiter& waiter)
        {
            if (!waiter.linked)
                return;
            if (waiter.next != nullptr)
                waiter.next->previous = waiter.previous;
            else
                m_last = waiter.previous;
            if (waiter.previous != nullptr)
                waiter.previous->next = waiter.next;
            waiter.previous = waiter.next = nullptr;
            waiter.linked = false;
        }

        void SignalLocked(Waiter& waiter)
        {
            // Remove before signaling: repeated notifications must select
            // distinct sleepers even if the first has not reached the CPU yet.
            UnlinkLocked(waiter);
            waiter.signaled = true;
            waiter.condition.notify_one();
        }

        template<class Predicate>
        bool WaitUntil(std::unique_lock<std::mutex>& stateLock, Waiter& waiter,
            std::chrono::steady_clock::time_point deadline, Predicate predicate)
        {
            while (!predicate())
            {
                // Repeated notifications can win even after wait_until's
                // deadline. Bound the whole operation, not just each park.
                if (deadline != std::chrono::steady_clock::time_point::max() &&
                    std::chrono::steady_clock::now() >= deadline)
                    return false;
                std::unique_lock signalLock(m_mutex);
                waiter.previous = m_last;
                waiter.next = nullptr;
                waiter.signaled = false;
                waiter.linked = true;
                if (m_last != nullptr)
                    m_last->next = &waiter;
                m_last = &waiter;

                // Register before releasing the queue-state lock. Notification
                // uses the signal lock and a persistent per-waiter bit, closing
                // the unlock-to-sleep window even if another worker takes the job.
                stateLock.unlock();
                bool notified = true;
                try
                {
                    if (deadline == std::chrono::steady_clock::time_point::max())
                        waiter.condition.wait(signalLock, [&] { return waiter.signaled; });
                    else
                        notified = waiter.condition.wait_until(signalLock, deadline, [&] { return waiter.signaled; });
                }
                catch (...)
                {
                    UnlinkLocked(waiter);
                    signalLock.unlock();
                    stateLock.lock();
                    throw;
                }
                UnlinkLocked(waiter); // Also handles a timeout away from the top.
                signalLock.unlock();
                // Never hold the signal lock while acquiring the state lock:
                // some producers notify while already holding the state lock.
                stateLock.lock();
                if (!notified)
                    return predicate();
            }
            return true;
        }

        std::mutex m_mutex;
        Waiter* m_last = nullptr;
    };
}
