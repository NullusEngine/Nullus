#pragma once

#include "BaseDef.h"
#include "TaskGraph/TaskEvent.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace NLS::Base::TaskGraph
{
    inline constexpr uint32_t kAutoTaskGraphWorkerCount = 0xffffffffu;

    enum class TaskPriority : uint8_t
    {
        Low = 0u,
        Normal,
        High
    };

    enum class NamedThread : uint8_t
    {
        Any = 0u,
        Game,
        Render,
        Rhi,
        Count
    };

    enum class TaskStatus : uint8_t
    {
        Pending = 0u,
        Succeeded,
        Failed,
        Cancelled,
        Count
    };

    enum class TaskGraphShutdownMode : uint8_t
    {
        DrainAcceptedWork = 0u,
        Immediate
    };

    struct NLS_BASE_API TaskHandle
    {
        uint64_t id = 0u;
        uint32_t generation = 0u;

        [[nodiscard]] bool IsValid() const noexcept { return id != 0u && generation != 0u; }
        [[nodiscard]] bool operator==(const TaskHandle& other) const noexcept = default;
    };

    struct NLS_BASE_API TaskGraphConfig
    {
        uint32_t workerCount = kAutoTaskGraphWorkerCount;
        bool forceInlineExecution = false;
        bool enableDiagnostics = true;
        /**
         * Number of terminal node records retained for handle inspection. Once the
         * bound is exceeded the oldest terminal records are erased, so per-frame
         * graphs cannot grow the task table without limit. Completion tokens that
         * were copied before recycling stay valid and observable.
         */
        size_t retainedTerminalTaskCount = 1024u;
    };

    struct NLS_BASE_API TaskDesc
    {
        std::function<void()> function;
        std::vector<TaskHandle> prerequisites;
        NamedThread targetThread = NamedThread::Any;
        TaskPriority priority = TaskPriority::Normal;
        std::string debugName;
        bool cancellable = true;
    };

    struct TaskCompletionState;

    /**
     * Copyable completion token. Waiting from a registered named thread pumps
     * that thread's queue while it waits, so dependency chains that terminate
     * on the same named thread cannot deadlock the caller.
     */
    class NLS_BASE_API TaskCompletion final
    {
    public:
        TaskCompletion() = default;

        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool IsComplete() const noexcept;
        [[nodiscard]] TaskStatus GetStatus() const noexcept;
        [[nodiscard]] TaskStatus Wait();
        [[nodiscard]] bool WaitFor(std::chrono::milliseconds timeout);

    private:
        friend class TaskGraph;
        explicit TaskCompletion(std::shared_ptr<TaskCompletionState> state);

        std::shared_ptr<TaskCompletionState> m_state;
    };

    class NLS_BASE_API TaskGraph final
    {
    public:
        explicit TaskGraph(TaskGraphConfig config = {});
        ~TaskGraph();

        TaskGraph(const TaskGraph&) = delete;
        TaskGraph& operator=(const TaskGraph&) = delete;

        bool Start();
        void Shutdown(TaskGraphShutdownMode mode = TaskGraphShutdownMode::DrainAcceptedWork);
        [[nodiscard]] bool IsStarted() const noexcept;

        bool RegisterCurrentThread(NamedThread thread);
        void UnregisterCurrentThread(NamedThread thread);
        [[nodiscard]] bool IsNamedThreadRegistered(NamedThread thread) const;
        [[nodiscard]] NamedThread GetCurrentNamedThread() const;

        TaskHandle Submit(TaskDesc desc);
        TaskHandle Submit(
            std::function<void()> function,
            NamedThread targetThread = NamedThread::Any,
            TaskPriority priority = TaskPriority::Normal,
            std::string debugName = {});

        size_t PumpCurrentThread(size_t maxTasks = 0u);
        size_t Pump(NamedThread thread, size_t maxTasks = 0u);
        bool WaitForWork(NamedThread thread, std::chrono::milliseconds timeout);
        void WakeNamedThread(NamedThread thread);

        TaskStatus Wait(TaskHandle handle);
        bool WaitFor(TaskHandle handle, std::chrono::milliseconds timeout);
        [[nodiscard]] TaskCompletion GetCompletion(TaskHandle handle) const;

        /**
         * Releases the captured payloads of terminal nodes while keeping their
         * handles, generations and completion state observable. Per-frame graphs
         * use this so completed node closures cannot pin frame-scoped resources.
         */
        size_t ReleaseTerminalTaskPayloads();

        /**
         * Node records currently retained: pending, running, and recent terminal
         * nodes kept for handle inspection. Bounded by
         * TaskGraphConfig::retainedTerminalTaskCount.
         */
        [[nodiscard]] size_t GetRetainedTaskCount() const;

        [[nodiscard]] uint32_t GetWorkerCount() const noexcept;
        [[nodiscard]] size_t GetPendingTaskCount() const;
        [[nodiscard]] size_t GetRunningTaskCount() const;
        [[nodiscard]] size_t GetNamedThreadQueueDepth(NamedThread thread) const;
        [[nodiscard]] bool IsWorkerThread() const noexcept;

    private:
        friend struct TaskCompletionState;

        struct Impl;
        std::shared_ptr<Impl> m_impl;
    };
}
