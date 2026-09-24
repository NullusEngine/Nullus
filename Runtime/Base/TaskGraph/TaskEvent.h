#pragma once

#include "BaseDef.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace NLS::Base::TaskGraph
{
    /**
     * Manual-reset event used by named-thread task queues and frame completion.
     *
     * The generation is capture-before-wait friendly: a trigger that races with
     * the transition from queue-drain to wait still advances the generation, so
     * WaitForGeneration cannot miss it.
     */
    class NLS_BASE_API TaskEvent final
    {
    public:
        TaskEvent() = default;
        TaskEvent(const TaskEvent&) = delete;
        TaskEvent& operator=(const TaskEvent&) = delete;

        uint64_t Trigger() noexcept;
        void Reset() noexcept;

        [[nodiscard]] uint64_t Generation() const noexcept;
        [[nodiscard]] bool IsTriggered() const noexcept;

        void Wait();
        bool WaitFor(std::chrono::milliseconds timeout);
        bool WaitForGeneration(uint64_t generation, std::chrono::milliseconds timeout);

    private:
        mutable std::mutex m_mutex;
        std::condition_variable m_condition;
        uint64_t m_generation = 0u;
        bool m_triggered = false;
    };
}
