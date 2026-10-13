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
     * on the same named thread cannot deadlock the caller. Waiting from a pool
     * worker assists the pool instead of parking, so a nested wait cannot
     * deadlock even when the graph owns a single worker.
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
        /** Releases a joined thread's registration from its lifecycle owner. */
        void UnregisterThread(NamedThread thread, std::thread::id threadId);
        [[nodiscard]] bool IsNamedThreadRegistered(NamedThread thread) const;
        [[nodiscard]] NamedThread GetCurrentNamedThread() const;

        TaskHandle Submit(TaskDesc desc);
        TaskHandle Submit(
            std::function<void()> function,
            NamedThread targetThread = NamedThread::Any,
            TaskPriority priority = TaskPriority::Normal,
            std::string debugName = {});

        /** Pumps the current named lane, or assists runnable work on a pool worker. */
        size_t PumpCurrentThread(size_t maxTasks = 0u);
        size_t Pump(NamedThread thread, size_t maxTasks = 0u);
        bool WaitForWork(NamedThread thread, std::chrono::milliseconds timeout);
        void WakeNamedThread(NamedThread thread);

        TaskStatus Wait(TaskHandle handle);
        bool WaitFor(TaskHandle handle, std::chrono::milliseconds timeout);
        [[nodiscard]] TaskCompletion GetCompletion(TaskHandle handle) const;

        /**
         * Requests cancellation of one task.
         *
         * A task that has not started yet is finalized as Cancelled and its
         * dependents are cancelled transitively. A task that already entered its
         * body cannot be preempted: the request is recorded so the body and any
         * observer can read IsCancellationRequested, and the terminal status is
         * whatever the body reports. Returns false when the handle is unknown,
         * stale, or already terminal.
         */
        bool RequestCancel(TaskHandle handle);
        [[nodiscard]] bool IsCancellationRequested(TaskHandle handle) const;

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

        /**
         * Scheduling telemetry: how many task bodies were executed from a
         * worker's own lane versus stolen from another lane. Used by the
         * work-stealing validation tests and by rendering telemetry.
         */
        [[nodiscard]] size_t GetWorkerStealCount() const;
        [[nodiscard]] size_t GetWorkerLocalExecutionCount() const;
        [[nodiscard]] size_t GetWorkerInjectionExecutionCount() const;

    private:
        friend struct TaskCompletionState;

        struct Impl;
        std::shared_ptr<Impl> m_impl;
    };
}
