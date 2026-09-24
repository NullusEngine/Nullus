#include "TaskGraph/TaskGraph.h"

#include "Profiling/Profiler.h"
#include "Threading/ExecutionThreadBudget.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace NLS::Base::TaskGraph
{
    namespace
    {
        thread_local const void* g_workerGraph = nullptr;

        constexpr size_t kPriorityQueueCount = 3u;

        size_t PriorityIndex(const TaskPriority priority)
        {
            return static_cast<size_t>(priority);
        }

        bool IsNamedThread(const NamedThread thread)
        {
            return thread >= NamedThread::Game && thread < NamedThread::Count;
        }

        size_t NamedThreadIndex(const NamedThread thread)
        {
            return static_cast<size_t>(thread) - static_cast<size_t>(NamedThread::Game);
        }

        const char* NamedThreadName(const NamedThread thread)
        {
            switch (thread)
            {
            case NamedThread::Game: return "Game Thread";
            case NamedThread::Render: return "Render Thread";
            case NamedThread::Rhi: return "RHI Thread";
            default: return "Task Worker";
            }
        }
    }

    struct TaskCompletionState final
    {
        mutable std::mutex mutex;
        std::condition_variable condition;
        TaskStatus status = TaskStatus::Pending;
        std::weak_ptr<TaskGraph::Impl> graph;

        [[nodiscard]] bool IsCompleteLocked() const noexcept
        {
            return status != TaskStatus::Pending;
        }

        [[nodiscard]] TaskStatus WaitFor(std::chrono::milliseconds timeout);
    };

    struct TaskGraph::Impl final
    {
        struct Node final
        {
            TaskHandle handle;
            TaskDesc desc;
            TaskStatus status = TaskStatus::Pending;
            size_t remainingDependencies = 0u;
            bool queued = false;
            bool executing = false;
            std::atomic_bool cancelRequested {false};
            std::vector<std::weak_ptr<Node>> dependents;
            std::shared_ptr<TaskCompletionState> completion;
        };

        explicit Impl(TaskGraphConfig inConfig)
            : config(std::move(inConfig))
        {
        }

        TaskGraphConfig config;
        uint32_t workerCount = 1u;
        mutable std::mutex mutex;
        std::condition_variable workerWake;
        std::array<std::deque<std::shared_ptr<Node>>, kPriorityQueueCount> workerQueues;
        std::array<std::deque<std::shared_ptr<Node>>, static_cast<size_t>(NamedThread::Count)> namedQueues;
        std::array<TaskEvent, static_cast<size_t>(NamedThread::Count)> namedEvents;
        std::array<std::unordered_set<std::thread::id>, static_cast<size_t>(NamedThread::Count)> namedThreadIds;
        std::unordered_map<uint64_t, std::shared_ptr<Node>> nodes;
        // Terminal nodes in finalization order. The front entries are recycled once
        // the configured retention bound is exceeded.
        std::deque<TaskHandle> terminalNodeOrder;
        std::vector<std::thread> workers;
        std::atomic<uint64_t> nextTaskId {1u};
        uint32_t nextGeneration = 1u;
        size_t runningTaskCount = 0u;
        bool started = false;
        bool acceptingWork = true;
        bool shuttingDown = false;
        bool immediateShutdown = false;

        static bool IsTerminal(const TaskStatus status)
        {
            return status != TaskStatus::Pending;
        }

        static void SetCompletionStatus(
            const std::shared_ptr<Node>& node,
            const TaskStatus status)
        {
            if (node == nullptr || node->completion == nullptr)
                return;

            std::lock_guard lock(node->completion->mutex);
            node->completion->status = status;
            node->completion->condition.notify_all();
        }

        void EnqueueReadyLocked(const std::shared_ptr<Node>& node)
        {
            if (node == nullptr || IsTerminal(node->status) || node->queued)
                return;

            node->queued = true;
            if (node->desc.targetThread == NamedThread::Any)
            {
                workerQueues[PriorityIndex(node->desc.priority)].push_back(node);
                workerWake.notify_one();
                return;
            }

            const size_t index = static_cast<size_t>(node->desc.targetThread);
            namedQueues[index].push_back(node);
            namedEvents[index].Trigger();
        }

        /**
         * Erases the oldest terminal records once the retention bound is exceeded.
         * Pending or running records are never erased, and completion tokens that
         * were already copied out stay valid because they own their state.
         */
        void PruneTerminalNodesLocked()
        {
            while (terminalNodeOrder.size() > config.retainedTerminalTaskCount)
            {
                const TaskHandle oldest = terminalNodeOrder.front();
                terminalNodeOrder.pop_front();

                const auto found = nodes.find(oldest.id);
                if (found == nodes.end() || found->second == nullptr)
                    continue;
                if (found->second->handle.generation != oldest.generation)
                    continue;
                if (!IsTerminal(found->second->status))
                    continue;

                nodes.erase(found);
            }
        }

        void FinalizeLocked(const std::shared_ptr<Node>& node, const TaskStatus status)
        {
            if (node == nullptr || IsTerminal(node->status))
                return;

            node->status = status;
            node->queued = false;
            node->executing = false;
            SetCompletionStatus(node, status);

            for (const auto& weakDependent : node->dependents)
            {
                const auto dependent = weakDependent.lock();
                if (dependent == nullptr || IsTerminal(dependent->status))
                    continue;

                if (status != TaskStatus::Succeeded)
                {
                    FinalizeLocked(dependent, TaskStatus::Cancelled);
                    continue;
                }

                if (dependent->remainingDependencies > 0u)
                    --dependent->remainingDependencies;
                if (dependent->remainingDependencies == 0u)
                    EnqueueReadyLocked(dependent);
            }

            // The node is terminal: drop its dependents and captured payloads, then
            // recycle the oldest retained records so per-frame graphs stay bounded.
            node->dependents.clear();
            node->desc.prerequisites.clear();
            node->desc.function = {};
            terminalNodeOrder.push_back(node->handle);
            PruneTerminalNodesLocked();
        }

        std::shared_ptr<Node> PopWorkerNodeLocked()
        {
            for (size_t index = 0u; index < workerQueues.size(); ++index)
            {
                auto& queue = workerQueues[workerQueues.size() - 1u - index];
                while (!queue.empty())
                {
                    auto node = std::move(queue.front());
                    queue.pop_front();
                    if (node == nullptr || IsTerminal(node->status))
                        continue;
                    node->queued = false;
                    node->executing = true;
                    ++runningTaskCount;
                    return node;
                }
            }
            return {};
        }

        std::shared_ptr<Node> PopNamedNodeLocked(const NamedThread thread)
        {
            if (!IsNamedThread(thread))
                return {};

            auto& queue = namedQueues[static_cast<size_t>(thread)];
            while (!queue.empty())
            {
                auto node = std::move(queue.front());
                queue.pop_front();
                if (node == nullptr || IsTerminal(node->status))
                    continue;
                node->queued = false;
                node->executing = true;
                ++runningTaskCount;
                return node;
            }
            return {};
        }

        void ExecuteTask(const std::shared_ptr<Node>& node)
        {
            if (node == nullptr)
                return;

            TaskStatus status = TaskStatus::Succeeded;
            if (node->cancelRequested.load(std::memory_order_acquire))
            {
                status = TaskStatus::Cancelled;
            }
            else
            {
                try
                {
                    NLS_PROFILE_NAMED_SCOPE(node->desc.debugName.empty() ? "TaskGraph::Task" : node->desc.debugName.c_str());
                    if (node->desc.function)
                        node->desc.function();
                }
                catch (...)
                {
                    status = TaskStatus::Failed;
                }
            }

            std::lock_guard lock(mutex);
            if (runningTaskCount > 0u)
                --runningTaskCount;
            FinalizeLocked(node, status);
        }

        void WorkerLoop(const uint32_t workerIndex)
        {
            g_workerGraph = this;
            const std::string workerName = "TaskGraph Worker " + std::to_string(workerIndex);
            NLS_PROFILE_REGISTER_THREAD(workerName);
            while (true)
            {
                std::shared_ptr<Node> node;
                {
                    std::unique_lock lock(mutex);
                    workerWake.wait(
                        lock,
                        [this]
                        {
                            return shuttingDown || PopWorkerNodeWouldSucceedLocked();
                        });

                    if (shuttingDown && !HasAnyWorkerQueueWorkLocked())
                        break;

                    node = PopWorkerNodeLocked();
                }

                if (node != nullptr)
                    ExecuteTask(node);
            }
            g_workerGraph = nullptr;
        }

        bool PopWorkerNodeWouldSucceedLocked() const
        {
            for (const auto& queue : workerQueues)
            {
                if (!queue.empty())
                    return true;
            }
            return false;
        }

        bool HasAnyWorkerQueueWorkLocked() const
        {
            return PopWorkerNodeWouldSucceedLocked();
        }

        size_t PumpNamedThread(const NamedThread thread, size_t maxTasks)
        {
            if (!IsNamedThread(thread))
                return 0u;

            size_t executedCount = 0u;
            while (maxTasks == 0u || executedCount < maxTasks)
            {
                std::shared_ptr<Node> node;
                {
                    std::lock_guard lock(mutex);
                    node = PopNamedNodeLocked(thread);
                }

                if (node == nullptr)
                    break;

                ExecuteTask(node);
                ++executedCount;
            }

            {
                std::lock_guard lock(mutex);
                if (namedQueues[static_cast<size_t>(thread)].empty())
                    namedEvents[static_cast<size_t>(thread)].Reset();
            }
            return executedCount;
        }

        TaskStatus WaitForCompletion(TaskCompletionState& state, std::chrono::milliseconds timeout)
        {
            const bool infiniteWait = timeout == std::chrono::milliseconds::max();
            const auto deadline = std::chrono::steady_clock::now() + timeout;

            while (true)
            {
                {
                    std::lock_guard lock(state.mutex);
                    if (state.IsCompleteLocked())
                        return state.status;
                }

                NamedThread currentThread = NamedThread::Any;
                {
                    std::lock_guard lock(mutex);
                    currentThread = GetCurrentNamedThread();
                }
                if (currentThread != NamedThread::Any)
                    PumpNamedThread(currentThread, 0u);

                std::unique_lock lock(state.mutex);
                if (state.IsCompleteLocked())
                    return state.status;

                if (infiniteWait)
                {
                    state.condition.wait(lock);
                    continue;
                }

                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline)
                    return state.status;

                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
                state.condition.wait_for(lock, (std::min)(remaining, std::chrono::milliseconds(1)));
            }
        }

        NamedThread GetCurrentNamedThread() const
        {
            const auto current = std::this_thread::get_id();
            for (size_t index = 0u; index < namedThreadIds.size(); ++index)
            {
                if (namedThreadIds[index].find(current) != namedThreadIds[index].end())
                    return static_cast<NamedThread>(index);
            }
            return NamedThread::Any;
        }
    };

    TaskStatus TaskCompletionState::WaitFor(const std::chrono::milliseconds timeout)
    {
        if (const auto owner = graph.lock())
            return owner->WaitForCompletion(*this, timeout);

        std::unique_lock lock(mutex);
        condition.wait_for(lock, timeout, [this] { return IsCompleteLocked(); });
        return status;
    }

    TaskCompletion::TaskCompletion(std::shared_ptr<TaskCompletionState> state)
        : m_state(std::move(state))
    {
    }

    bool TaskCompletion::IsValid() const noexcept
    {
        return m_state != nullptr;
    }

    bool TaskCompletion::IsComplete() const noexcept
    {
        if (m_state == nullptr)
            return true;
        std::lock_guard lock(m_state->mutex);
        return m_state->IsCompleteLocked();
    }

    TaskStatus TaskCompletion::GetStatus() const noexcept
    {
        if (m_state == nullptr)
            return TaskStatus::Cancelled;
        std::lock_guard lock(m_state->mutex);
        return m_state->status;
    }

    TaskStatus TaskCompletion::Wait()
    {
        if (m_state == nullptr)
            return TaskStatus::Cancelled;
        return m_state->WaitFor(std::chrono::milliseconds::max());
    }

    bool TaskCompletion::WaitFor(const std::chrono::milliseconds timeout)
    {
        if (m_state == nullptr)
            return true;
        return m_state->WaitFor(timeout) != TaskStatus::Pending;
    }

    TaskGraph::TaskGraph(TaskGraphConfig config)
        : m_impl(std::make_shared<Impl>(std::move(config)))
    {
    }

    TaskGraph::~TaskGraph()
    {
        Shutdown(TaskGraphShutdownMode::Immediate);
    }

    bool TaskGraph::Start()
    {
        if (m_impl == nullptr)
            return false;

        std::lock_guard lock(m_impl->mutex);
        if (m_impl->started)
            return true;
        if (m_impl->shuttingDown)
            return false;

        // A drained shutdown leaves the graph reusable so drivers and tests can
        // restart the process-lifetime global graph in subsequent scenarios.
        m_impl->acceptingWork = true;

        const uint32_t hardware = std::max(1u, std::thread::hardware_concurrency());
        m_impl->workerCount = m_impl->config.workerCount == kAutoTaskGraphWorkerCount
            ? NLS::Base::Threading::ResolveExecutionPoolBudget(hardware).taskGraphWorkerCount
            : std::max(1u, m_impl->config.workerCount);

        m_impl->started = true;
        try
        {
            m_impl->workers.reserve(m_impl->workerCount);
            for (uint32_t workerIndex = 0u; workerIndex < m_impl->workerCount; ++workerIndex)
            {
                m_impl->workers.emplace_back(
                    [impl = m_impl.get(), workerIndex] { impl->WorkerLoop(workerIndex); });
            }
        }
        catch (...)
        {
            m_impl->started = false;
            m_impl->acceptingWork = false;
            m_impl->shuttingDown = true;
            m_impl->workerWake.notify_all();
            return false;
        }
        return true;
    }

    void TaskGraph::Shutdown(const TaskGraphShutdownMode mode)
    {
        if (m_impl == nullptr)
            return;

        std::vector<std::thread> workers;
        {
            std::lock_guard lock(m_impl->mutex);
            if (!m_impl->started && !m_impl->acceptingWork)
                return;

            m_impl->acceptingWork = false;
            m_impl->shuttingDown = true;
            m_impl->immediateShutdown = mode == TaskGraphShutdownMode::Immediate;

            if (mode == TaskGraphShutdownMode::Immediate)
            {
                for (auto& [id, node] : m_impl->nodes)
                {
                    (void)id;
                    m_impl->FinalizeLocked(node, TaskStatus::Cancelled);
                }
                for (auto& queue : m_impl->workerQueues)
                    queue.clear();
                for (auto& queue : m_impl->namedQueues)
                    queue.clear();
            }

            workers = std::move(m_impl->workers);
            m_impl->workerWake.notify_all();
        }

        if (mode == TaskGraphShutdownMode::DrainAcceptedWork)
        {
            while (true)
            {
                for (const NamedThread thread : {NamedThread::Game, NamedThread::Render, NamedThread::Rhi})
                    m_impl->PumpNamedThread(thread, 0u);

                {
                    std::lock_guard lock(m_impl->mutex);
                    const bool hasPending = std::any_of(
                        m_impl->nodes.begin(),
                        m_impl->nodes.end(),
                        [](const auto& entry) { return entry.second != nullptr && !Impl::IsTerminal(entry.second->status); });
                    if (!hasPending && !m_impl->PopWorkerNodeWouldSucceedLocked())
                        break;
                }

                m_impl->workerWake.notify_all();
                std::this_thread::yield();
            }
        }

        for (auto& worker : workers)
        {
            if (worker.joinable())
                worker.join();
        }

        {
            std::lock_guard lock(m_impl->mutex);
            m_impl->nodes.clear();
            for (auto& queue : m_impl->workerQueues)
                queue.clear();
            for (auto& queue : m_impl->namedQueues)
                queue.clear();
            m_impl->runningTaskCount = 0u;
            m_impl->started = false;
            m_impl->shuttingDown = false;
            m_impl->immediateShutdown = false;
            m_impl->acceptingWork = true;
        }
    }

    bool TaskGraph::IsStarted() const noexcept
    {
        if (m_impl == nullptr)
            return false;
        std::lock_guard lock(m_impl->mutex);
        return m_impl->started;
    }

    bool TaskGraph::RegisterCurrentThread(const NamedThread thread)
    {
        if (m_impl == nullptr || !IsNamedThread(thread))
            return false;

        std::lock_guard lock(m_impl->mutex);
        const size_t index = static_cast<size_t>(thread);
        const auto current = std::this_thread::get_id();
        m_impl->namedThreadIds[index].insert(current);
        return true;
    }

    void TaskGraph::UnregisterCurrentThread(const NamedThread thread)
    {
        if (m_impl == nullptr || !IsNamedThread(thread))
            return;

        std::lock_guard lock(m_impl->mutex);
        const size_t index = static_cast<size_t>(thread);
        m_impl->namedThreadIds[index].erase(std::this_thread::get_id());
    }

    bool TaskGraph::IsNamedThreadRegistered(const NamedThread thread) const
    {
        if (m_impl == nullptr || !IsNamedThread(thread))
            return false;
        std::lock_guard lock(m_impl->mutex);
        return !m_impl->namedThreadIds[static_cast<size_t>(thread)].empty();
    }

    NamedThread TaskGraph::GetCurrentNamedThread() const
    {
        if (m_impl == nullptr)
            return NamedThread::Any;
        std::lock_guard lock(m_impl->mutex);
        return m_impl->GetCurrentNamedThread();
    }

    TaskHandle TaskGraph::Submit(TaskDesc desc)
    {
        if (m_impl == nullptr || !desc.function)
            return {};

        if (!IsStarted() && !Start())
            return {};

        auto node = std::make_shared<Impl::Node>();
        node->handle.id = m_impl->nextTaskId.fetch_add(1u, std::memory_order_relaxed);
        node->handle.generation = m_impl->nextGeneration++;
        node->desc = std::move(desc);
        node->completion = std::make_shared<TaskCompletionState>();
        node->completion->graph = m_impl;

        bool enqueueInline = false;
        {
            std::lock_guard lock(m_impl->mutex);
            if (!m_impl->acceptingWork || m_impl->shuttingDown)
                return {};

            const uint64_t nodeId = node->handle.id;
            std::vector<uint64_t> prerequisiteIds;
            prerequisiteIds.reserve(node->desc.prerequisites.size());
            for (const TaskHandle prerequisite : node->desc.prerequisites)
            {
                if (prerequisite.id == nodeId)
                    return {};

                if (std::find(prerequisiteIds.begin(), prerequisiteIds.end(), prerequisite.id) != prerequisiteIds.end())
                    return {};
                prerequisiteIds.push_back(prerequisite.id);

                const auto found = m_impl->nodes.find(prerequisite.id);
                if (found == m_impl->nodes.end() ||
                    found->second == nullptr ||
                    found->second->handle.generation != prerequisite.generation)
                {
                    return {};
                }

                const auto& dependency = found->second;
                if (dependency->status == TaskStatus::Pending)
                {
                    ++node->remainingDependencies;
                    dependency->dependents.push_back(node);
                }
                else if (dependency->status != TaskStatus::Succeeded)
                {
                    m_impl->nodes.emplace(nodeId, node);
                    m_impl->FinalizeLocked(node, TaskStatus::Cancelled);
                    return node->handle;
                }
            }

            m_impl->nodes.emplace(nodeId, node);
            if (node->remainingDependencies == 0u)
            {
                if (m_impl->config.forceInlineExecution)
                    enqueueInline = true;
                else
                    m_impl->EnqueueReadyLocked(node);
            }
        }

        if (enqueueInline)
        {
            m_impl->ExecuteTask(node);
        }
        return node->handle;
    }

    TaskHandle TaskGraph::Submit(
        std::function<void()> function,
        const NamedThread targetThread,
        const TaskPriority priority,
        std::string debugName)
    {
        TaskDesc desc;
        desc.function = std::move(function);
        desc.targetThread = targetThread;
        desc.priority = priority;
        desc.debugName = std::move(debugName);
        return Submit(std::move(desc));
    }

    size_t TaskGraph::PumpCurrentThread(const size_t maxTasks)
    {
        return Pump(GetCurrentNamedThread(), maxTasks);
    }

    size_t TaskGraph::Pump(const NamedThread thread, const size_t maxTasks)
    {
        if (m_impl == nullptr || !IsNamedThread(thread))
            return 0u;
        return m_impl->PumpNamedThread(thread, maxTasks);
    }

    bool TaskGraph::WaitForWork(const NamedThread thread, const std::chrono::milliseconds timeout)
    {
        if (m_impl == nullptr || !IsNamedThread(thread))
            return false;

        uint64_t observedGeneration = 0u;
        {
            std::lock_guard lock(m_impl->mutex);
            if (!m_impl->namedQueues[static_cast<size_t>(thread)].empty())
                return true;
            observedGeneration = m_impl->namedEvents[static_cast<size_t>(thread)].Generation();
        }
        return m_impl->namedEvents[static_cast<size_t>(thread)].WaitForGeneration(observedGeneration, timeout);
    }

    void TaskGraph::WakeNamedThread(const NamedThread thread)
    {
        if (m_impl == nullptr || !IsNamedThread(thread))
            return;
        m_impl->namedEvents[static_cast<size_t>(thread)].Trigger();
    }

    TaskStatus TaskGraph::Wait(const TaskHandle handle)
    {
        return WaitFor(handle, std::chrono::milliseconds::max()) ? GetCompletion(handle).GetStatus() : TaskStatus::Pending;
    }

    bool TaskGraph::WaitFor(const TaskHandle handle, const std::chrono::milliseconds timeout)
    {
        if (m_impl == nullptr || !handle.IsValid())
            return false;

        std::shared_ptr<TaskCompletionState> completion;
        {
            std::lock_guard lock(m_impl->mutex);
            const auto found = m_impl->nodes.find(handle.id);
            if (found == m_impl->nodes.end() || found->second == nullptr ||
                found->second->handle.generation != handle.generation)
            {
                return false;
            }
            completion = found->second->completion;
        }

        return completion->WaitFor(timeout) != TaskStatus::Pending;
    }

    TaskCompletion TaskGraph::GetCompletion(const TaskHandle handle) const
    {
        if (m_impl == nullptr || !handle.IsValid())
            return {};

        std::lock_guard lock(m_impl->mutex);
        const auto found = m_impl->nodes.find(handle.id);
        if (found == m_impl->nodes.end() || found->second == nullptr ||
            found->second->handle.generation != handle.generation)
        {
            return {};
        }
        return TaskCompletion(found->second->completion);
    }

    size_t TaskGraph::ReleaseTerminalTaskPayloads()
    {
        if (m_impl == nullptr)
            return 0u;

        std::lock_guard lock(m_impl->mutex);
        size_t releasedCount = 0u;
        for (auto& [id, node] : m_impl->nodes)
        {
            (void)id;
            if (node == nullptr || !Impl::IsTerminal(node->status) || node->executing)
                continue;
            if (!node->desc.function)
                continue;

            node->desc.function = {};
            node->desc.prerequisites.clear();
            node->desc.debugName.clear();
            node->desc.cancellable = false;
            ++releasedCount;
        }
        return releasedCount;
    }

    uint32_t TaskGraph::GetWorkerCount() const noexcept
    {
        return m_impl != nullptr ? m_impl->workerCount : 0u;
    }

    size_t TaskGraph::GetRetainedTaskCount() const
    {
        if (m_impl == nullptr)
            return 0u;
        std::lock_guard lock(m_impl->mutex);
        return m_impl->nodes.size();
    }

    size_t TaskGraph::GetPendingTaskCount() const
    {
        if (m_impl == nullptr)
            return 0u;
        std::lock_guard lock(m_impl->mutex);
        return static_cast<size_t>(std::count_if(
            m_impl->nodes.begin(),
            m_impl->nodes.end(),
            [](const auto& entry) { return entry.second != nullptr && !Impl::IsTerminal(entry.second->status); }));
    }

    size_t TaskGraph::GetRunningTaskCount() const
    {
        if (m_impl == nullptr)
            return 0u;
        std::lock_guard lock(m_impl->mutex);
        return m_impl->runningTaskCount;
    }

    size_t TaskGraph::GetNamedThreadQueueDepth(const NamedThread thread) const
    {
        if (m_impl == nullptr || !IsNamedThread(thread))
            return 0u;
        std::lock_guard lock(m_impl->mutex);
        return m_impl->namedQueues[static_cast<size_t>(thread)].size();
    }

    bool TaskGraph::IsWorkerThread() const noexcept
    {
        return g_workerGraph == m_impl.get();
    }
}
