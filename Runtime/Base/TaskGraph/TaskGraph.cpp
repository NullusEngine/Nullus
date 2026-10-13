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
        constexpr size_t kPriorityQueueCount = 3u;
        constexpr uint32_t kInvalidWorkerIndex = 0xffffffffu;

        // Node lifecycle is claimed with a CAS so a cancellation that races with
        // a worker pop can never hand the same node to two executors.
        constexpr uint8_t kLifecycleQueued = 0u;
        constexpr uint8_t kLifecycleClaimed = 1u;
        constexpr uint8_t kLifecycleTerminal = 2u;

        /** Where a worker obtained the task it is about to execute. */
        enum class ReadySource : uint8_t
        {
            Local = 0u,
            Stolen,
            Injection
        };

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

        /**
         * Worker identity for the calling thread. Nested submissions from a task
         * body push into the owning worker lane, and a worker that blocks on a
         * completion assists the pool instead of parking.
         */
        struct WorkerIdentity final
        {
            const void* graph = nullptr;
            uint32_t workerIndex = kInvalidWorkerIndex;
        };

        thread_local WorkerIdentity g_workerIdentity;
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
            std::atomic<uint8_t> lifecycle { kLifecycleQueued };
            std::atomic_bool cancelRequested {false};
            std::vector<std::weak_ptr<Node>> dependents;
            std::shared_ptr<TaskCompletionState> completion;
        };

        /**
         * One work-stealing lane per worker. The lane mutex is the only lock a
         * worker takes to pop its own work, so the steady-state scheduling path no
         * longer contends on a single shared queue mutex.
         */
        struct Lane final
        {
            mutable std::mutex mutex;
            std::array<std::deque<std::shared_ptr<Node>>, kPriorityQueueCount> queues;
        };

        explicit Impl(TaskGraphConfig inConfig)
            : config(std::move(inConfig))
        {
        }

        TaskGraphConfig config;
        uint32_t workerCount = 1u;
        mutable std::mutex mutex;
        std::condition_variable workerWake;
        // Local lanes plus the shared injection queue. Every push takes `mutex`
        // first, so the idle-worker predicate can never miss work.
        std::vector<std::unique_ptr<Lane>> lanes;
        mutable std::mutex injectionMutex;
        std::array<std::deque<std::shared_ptr<Node>>, kPriorityQueueCount> injectionQueues;
        std::array<std::deque<std::shared_ptr<Node>>, static_cast<size_t>(NamedThread::Count)> namedQueues;
        std::array<TaskEvent, static_cast<size_t>(NamedThread::Count)> namedEvents;
        std::array<std::unordered_set<std::thread::id>, static_cast<size_t>(NamedThread::Count)> namedThreadIds;
        std::unordered_map<uint64_t, std::shared_ptr<Node>> nodes;
        // Terminal nodes in finalization order. The front entries are recycled once
        // the configured retention bound is exceeded.
        std::deque<TaskHandle> terminalNodeOrder;
        std::vector<std::thread> workers;
        std::atomic<uint64_t> nextTaskId { 1u };
        std::atomic<size_t> runningTaskCount { 0u };
        std::atomic<size_t> localExecutionCount { 0u };
        std::atomic<size_t> stealExecutionCount { 0u };
        std::atomic<size_t> injectionExecutionCount { 0u };
        uint32_t nextGeneration = 1u;
        bool started = false;
        bool acceptingWork = true;
        bool shuttingDown = false;
        bool immediateShutdown = false;

        static bool IsTerminal(const TaskStatus status)
        {
            return status != TaskStatus::Pending;
        }

        static bool IsTerminalLifecycle(const uint8_t lifecycle)
        {
            return lifecycle == kLifecycleTerminal;
        }

        /**
         * Drops a finished node's captured payload. A node whose body is still
         * executing on another thread must keep its std::function alive, so the
         * release is only allowed for the thread that ran the body or for a node
         * that was never claimed.
         */
        static void ReleaseNodePayload(
            const std::shared_ptr<Node>& node,
            const bool bodyFinishedOnThisThread)
        {
            if (node == nullptr)
                return;
            if (!bodyFinishedOnThisThread &&
                node->lifecycle.load(std::memory_order_acquire) == kLifecycleClaimed)
            {
                return;
            }

            node->dependents.clear();
            node->desc.prerequisites.clear();
            node->desc.function = {};
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

        void PushLaneLocked(const uint32_t laneIndex, const std::shared_ptr<Node>& node)
        {
            Lane& lane = *lanes[laneIndex];
            {
                std::lock_guard lock(lane.mutex);
                lane.queues[PriorityIndex(node->desc.priority)].push_back(node);
            }
            // A nested submission parked on this lane can be stolen by an idle
            // worker, so wake the pool even though the submitting worker is busy.
            workerWake.notify_all();
        }

        void PushInjectionLocked(const std::shared_ptr<Node>& node)
        {
            {
                std::lock_guard lock(injectionMutex);
                injectionQueues[PriorityIndex(node->desc.priority)].push_back(node);
            }
            workerWake.notify_all();
        }

        /**
         * Routes a ready node to a worker lane, the injection queue, or a named
         * thread queue. Callers hold `mutex` so the idle-worker predicate observes
         * new work atomically with the wake-up.
         */
        void EnqueueReadyLocked(const std::shared_ptr<Node>& node)
        {
            if (node == nullptr || IsTerminal(node->status))
                return;
            if (IsTerminalLifecycle(node->lifecycle.load(std::memory_order_acquire)))
                return;

            if (node->desc.targetThread == NamedThread::Any)
            {
                // Nested submissions stay on the submitting worker's lane so they
                // are popped with the best cache locality; external submissions
                // land in the shared injection queue and are stolen from there.
                const bool submittedFromThisGraphWorker =
                    g_workerIdentity.graph == static_cast<const void*>(this) &&
                    g_workerIdentity.workerIndex < lanes.size() &&
                    !lanes.empty();
                if (submittedFromThisGraphWorker)
                    PushLaneLocked(g_workerIdentity.workerIndex, node);
                else
                    PushInjectionLocked(node);
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

        /**
         * Marks a node terminal and cascades through its dependents. The cascade is
         * iterative so a long dependency chain cannot overflow the stack, and every
         * node whose dependencies reach zero is enqueued after the graph lock is
         * released to keep the critical section short.
         */
        void Finalize(const std::shared_ptr<Node>& root, const TaskStatus status)
        {
            if (root == nullptr)
                return;

            std::vector<std::shared_ptr<Node>> readyNodes;
            std::vector<std::pair<std::shared_ptr<Node>, TaskStatus>> terminalNodes;
            {
                std::lock_guard lock(mutex);

                std::vector<std::pair<std::shared_ptr<Node>, TaskStatus>> pending;
                pending.emplace_back(root, status);

                while (!pending.empty())
                {
                    auto [node, nodeStatus] = std::move(pending.back());
                    pending.pop_back();

                    if (node == nullptr || IsTerminal(node->status))
                        continue;

                    node->status = nodeStatus;
                    // Exchange first so the release decision sees the pre-terminal
                    // state: a body already claimed by another thread must keep its
                    // std::function alive until that thread returns.
                    const uint8_t previousLifecycle =
                        node->lifecycle.exchange(kLifecycleTerminal, std::memory_order_acq_rel);
                    terminalNodes.emplace_back(node, nodeStatus);

                    for (const auto& weakDependent : node->dependents)
                    {
                        const auto dependent = weakDependent.lock();
                        if (dependent == nullptr || IsTerminal(dependent->status))
                            continue;

                        if (nodeStatus != TaskStatus::Succeeded)
                        {
                            pending.emplace_back(dependent, TaskStatus::Cancelled);
                            continue;
                        }

                        if (dependent->remainingDependencies > 0u)
                            --dependent->remainingDependencies;
                        if (dependent->remainingDependencies == 0u)
                            readyNodes.push_back(dependent);
                    }

                    // The node is terminal: drop its dependents and captured payloads,
                    // then recycle the oldest retained records so per-frame graphs stay
                    // bounded.
                    // Only the node this thread just executed may release a payload
                    // that was claimed; a cascaded dependent is never running.
                    ReleaseNodePayload(
                        node,
                        node == root || previousLifecycle != kLifecycleClaimed);
                    terminalNodeOrder.push_back(node->handle);
                    PruneTerminalNodesLocked();
                }

                for (const auto& ready : readyNodes)
                    EnqueueReadyLocked(ready);
            }

            // Publish terminal state for every node this call finalized, including
            // the transitively cancelled dependents. A waiter that observed the
            // node before this point would otherwise never be released.
            for (const auto& [node, nodeStatus] : terminalNodes)
                SetCompletionStatus(node, nodeStatus);
        }

        /**
         * Pops the newest task from a lane (LIFO). Newest-first keeps the hottest
         * operand data in cache for the worker that produced the work.
         */
        std::shared_ptr<Node> PopLane(const uint32_t laneIndex)
        {
            Lane& lane = *lanes[laneIndex];
            std::lock_guard lock(lane.mutex);
            for (size_t index = 0u; index < lane.queues.size(); ++index)
            {
                auto& queue = lane.queues[lane.queues.size() - 1u - index];
                while (!queue.empty())
                {
                    auto node = std::move(queue.back());
                    queue.pop_back();
                    if (node == nullptr || IsTerminalLifecycle(
                            node->lifecycle.load(std::memory_order_acquire)))
                    {
                        continue;
                    }
                    return node;
                }
            }
            return {};
        }

        /**
         * Steals the oldest task from another lane (FIFO). Oldest-first avoids
         * fighting the victim for the item it is about to pop.
         */
        std::shared_ptr<Node> StealLane(const uint32_t laneIndex)
        {
            Lane& lane = *lanes[laneIndex];
            std::lock_guard lock(lane.mutex);
            for (size_t index = 0u; index < lane.queues.size(); ++index)
            {
                auto& queue = lane.queues[lane.queues.size() - 1u - index];
                while (!queue.empty())
                {
                    auto node = std::move(queue.front());
                    queue.pop_front();
                    if (node == nullptr || IsTerminalLifecycle(
                            node->lifecycle.load(std::memory_order_acquire)))
                    {
                        continue;
                    }
                    return node;
                }
            }
            return {};
        }

        std::shared_ptr<Node> PopInjection()
        {
            std::lock_guard lock(injectionMutex);
            for (size_t index = 0u; index < injectionQueues.size(); ++index)
            {
                auto& queue = injectionQueues[injectionQueues.size() - 1u - index];
                while (!queue.empty())
                {
                    auto node = std::move(queue.front());
                    queue.pop_front();
                    if (node == nullptr || IsTerminalLifecycle(
                            node->lifecycle.load(std::memory_order_acquire)))
                    {
                        continue;
                    }
                    return node;
                }
            }
            return {};
        }

        /**
         * Claims one runnable task for `workerIndex`: own lane first, then a
         * round-robin steal scan of the other lanes, then the shared injection
         * queue. Nothing here takes `mutex`, so workers do not serialize on the
         * graph lock while scheduling.
         */
        std::shared_ptr<Node> ClaimReadyNode(
            const uint32_t workerIndex,
            ReadySource* outSource)
        {
            if (outSource != nullptr)
                *outSource = ReadySource::Injection;

            if (auto node = PopLane(workerIndex))
            {
                if (outSource != nullptr)
                    *outSource = ReadySource::Local;
                return node;
            }

            const uint32_t laneCount = static_cast<uint32_t>(lanes.size());
            for (uint32_t offset = 1u; offset < laneCount; ++offset)
            {
                const uint32_t victim = (workerIndex + offset) % laneCount;
                if (auto node = StealLane(victim))
                {
                    if (outSource != nullptr)
                        *outSource = ReadySource::Stolen;
                    return node;
                }
            }

            return PopInjection();
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
                return node;
            }
            return {};
        }

        void ExecuteTaskBody(const std::shared_ptr<Node>& node)
        {
            TaskStatus status = TaskStatus::Succeeded;
            if (node->cancelRequested.load(std::memory_order_acquire))
            {
                status = TaskStatus::Cancelled;
            }
            else
            {
                try
                {
                    NLS_PROFILE_NAMED_SCOPE(
                        node->desc.debugName.empty() ? "TaskGraph::Task" : node->desc.debugName.c_str());
                    if (node->desc.function)
                        node->desc.function();
                }
                catch (...)
                {
                    status = TaskStatus::Failed;
                }
            }

            Finalize(node, status);
        }

        /**
         * Runs at most one ready task and reports whether one was claimed. The
         * worker loop and the blocked-worker assist path share this entry point.
         */
        bool TryExecuteOneReadyTask(const uint32_t workerIndex)
        {
            while (true)
            {
                ReadySource source = ReadySource::Injection;
                auto node = ClaimReadyNode(workerIndex, &source);
                if (node == nullptr)
                    return false;

                uint8_t expected = kLifecycleQueued;
                if (!node->lifecycle.compare_exchange_strong(
                        expected,
                        kLifecycleClaimed,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    // Cancelled or already claimed elsewhere: keep looking.
                    continue;
                }

                if (source == ReadySource::Local)
                    localExecutionCount.fetch_add(1u, std::memory_order_relaxed);
                else if (source == ReadySource::Stolen)
                    stealExecutionCount.fetch_add(1u, std::memory_order_relaxed);
                else
                    injectionExecutionCount.fetch_add(1u, std::memory_order_relaxed);
                runningTaskCount.fetch_add(1u, std::memory_order_acq_rel);
                ExecuteTaskBody(node);
                runningTaskCount.fetch_sub(1u, std::memory_order_acq_rel);
                return true;
            }
        }

        void WorkerLoop(const uint32_t workerIndex)
        {
            const WorkerIdentity previousIdentity = g_workerIdentity;
            g_workerIdentity.graph = static_cast<const void*>(this);
            g_workerIdentity.workerIndex = workerIndex;

            const std::string workerName = "TaskGraph Worker " + std::to_string(workerIndex);
            NLS_PROFILE_REGISTER_THREAD(workerName);

            while (true)
            {
                if (TryExecuteOneReadyTask(workerIndex))
                    continue;

                std::unique_lock lock(mutex);
                // Every queue push happens under `mutex` and notifies, so a plain
                // predicate wait cannot miss work while idle workers stay parked.
                workerWake.wait(
                    lock,
                    [this]
                    {
                        return shuttingDown || HasRunnableWorkLocked();
                    });

                if (shuttingDown && !HasRunnableWorkLocked())
                    break;
            }

            g_workerIdentity = previousIdentity;
        }

        bool HasRunnableWorkLocked() const
        {
            for (const auto& lane : lanes)
            {
                if (lane == nullptr)
                    continue;
                std::lock_guard lock(lane->mutex);
                for (const auto& queue : lane->queues)
                {
                    if (!queue.empty())
                        return true;
                }
            }

            {
                std::lock_guard lock(injectionMutex);
                for (const auto& queue : injectionQueues)
                {
                    if (!queue.empty())
                        return true;
                }
            }

            return false;
        }

        bool HasNamedWorkLocked() const
        {
            for (const auto& queue : namedQueues)
            {
                if (!queue.empty())
                    return true;
            }
            return false;
        }

        void ClearQueuesLocked()
        {
            for (auto& lane : lanes)
            {
                if (lane == nullptr)
                    continue;
                std::lock_guard lock(lane->mutex);
                for (auto& queue : lane->queues)
                    queue.clear();
            }

            {
                std::lock_guard lock(injectionMutex);
                for (auto& queue : injectionQueues)
                    queue.clear();
            }

            for (auto& queue : namedQueues)
                queue.clear();
        }

        /**
         * Immediate shutdown: every unstarted node is cancelled and every queue is
         * dropped. A body that is already executing cannot be preempted, so the
         * caller still joins it.
         */
        void CancelAllPendingLocked()
        {
            for (auto& [id, node] : nodes)
            {
                (void)id;
                if (node == nullptr || IsTerminal(node->status))
                    continue;

                node->status = TaskStatus::Cancelled;
                const uint8_t previousLifecycle =
                    node->lifecycle.exchange(kLifecycleTerminal, std::memory_order_acq_rel);
                // A body that is already executing finishes on its own thread; its
                // captured payload stays alive until then.
                if (previousLifecycle != kLifecycleClaimed)
                    ReleaseNodePayload(node, true);
                terminalNodeOrder.push_back(node->handle);
                PruneTerminalNodesLocked();

                if (node->completion != nullptr)
                {
                    std::lock_guard lock(node->completion->mutex);
                    node->completion->status = TaskStatus::Cancelled;
                    node->completion->condition.notify_all();
                }
            }
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

                uint8_t expected = kLifecycleQueued;
                if (!node->lifecycle.compare_exchange_strong(
                        expected,
                        kLifecycleClaimed,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    continue;
                }

                runningTaskCount.fetch_add(1u, std::memory_order_acq_rel);
                ExecuteTaskBody(node);
                runningTaskCount.fetch_sub(1u, std::memory_order_acq_rel);
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
            const auto deadline = infiniteWait
                ? std::chrono::steady_clock::time_point::max()
                : std::chrono::steady_clock::now() + timeout;

            while (true)
            {
                {
                    std::lock_guard lock(state.mutex);
                    if (state.IsCompleteLocked())
                        return state.status;
                }

                // A blocked pool worker cooperates instead of parking, so a nested
                // wait cannot deadlock even when the graph owns a single worker.
                AssistWhileWaiting();

                NamedThread currentThread = NamedThread::Any;
                if (!IsCurrentThreadGraphWorker())
                {
                    std::lock_guard lock(mutex);
                    currentThread = GetCurrentNamedThreadLocked();
                }
                if (currentThread != NamedThread::Any)
                    PumpNamedThread(currentThread, 0u);

                std::unique_lock lock(state.mutex);
                if (state.IsCompleteLocked())
                    return state.status;

                if (infiniteWait)
                {
                    state.condition.wait_for(lock, std::chrono::milliseconds(1));
                    continue;
                }

                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline)
                    return state.status;

                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
                state.condition.wait_for(lock, (std::min)(remaining, std::chrono::milliseconds(1)));
            }
        }

        bool IsCurrentThreadGraphWorker() const
        {
            return g_workerIdentity.graph == static_cast<const void*>(this) &&
                g_workerIdentity.workerIndex < workerCount;
        }

        /**
         * Runs runnable `Any` work while the calling worker waits. Only workers
         * assist: an unregistered caller keeps the documented contract that
         * waiting never executes worker tasks.
         */
        void AssistWhileWaiting()
        {
            if (!IsCurrentThreadGraphWorker())
                return;

            (void)TryExecuteOneReadyTask(g_workerIdentity.workerIndex);
        }

        NamedThread GetCurrentNamedThreadLocked() const
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
        ++m_impl->nextGeneration;
        if (m_impl->nextGeneration == 0u)
            ++m_impl->nextGeneration;

        const uint32_t hardware = std::max(1u, std::thread::hardware_concurrency());
        m_impl->workerCount = m_impl->config.workerCount == kAutoTaskGraphWorkerCount
            ? NLS::Base::Threading::ResolveExecutionPoolBudget(hardware).taskGraphWorkerCount
            : std::max(1u, m_impl->config.workerCount);

        m_impl->lanes.clear();
        m_impl->lanes.reserve(m_impl->workerCount);
        for (uint32_t workerIndex = 0u; workerIndex < m_impl->workerCount; ++workerIndex)
            m_impl->lanes.push_back(std::make_unique<Impl::Lane>());

        m_impl->localExecutionCount.store(0u, std::memory_order_relaxed);
        m_impl->stealExecutionCount.store(0u, std::memory_order_relaxed);
        m_impl->injectionExecutionCount.store(0u, std::memory_order_relaxed);

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
                m_impl->CancelAllPendingLocked();
                m_impl->ClearQueuesLocked();
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
                    if (!hasPending &&
                        !m_impl->HasRunnableWorkLocked() &&
                        !m_impl->HasNamedWorkLocked())
                    {
                        break;
                    }
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
            m_impl->terminalNodeOrder.clear();
            m_impl->ClearQueuesLocked();
            m_impl->lanes.clear();
            m_impl->runningTaskCount.store(0u, std::memory_order_release);
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
        UnregisterThread(thread, std::this_thread::get_id());
    }

    void TaskGraph::UnregisterThread(const NamedThread thread, const std::thread::id threadId)
    {
        if (m_impl == nullptr || !IsNamedThread(thread))
            return;

        std::lock_guard lock(m_impl->mutex);
        const size_t index = static_cast<size_t>(thread);
        m_impl->namedThreadIds[index].erase(threadId);
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
        return m_impl->GetCurrentNamedThreadLocked();
    }

    TaskHandle TaskGraph::Submit(TaskDesc desc)
    {
        if (m_impl == nullptr || !desc.function)
            return {};

        if (!IsStarted() && !Start())
            return {};

        std::shared_ptr<Impl::Node> node;
        TaskHandle handle;
        bool dependencyAlreadyFailed = false;
        bool runInline = false;
        {
            std::lock_guard lock(m_impl->mutex);
            if (!m_impl->acceptingWork || m_impl->shuttingDown)
                return {};
            if (!IsNamedThread(desc.targetThread) && desc.targetThread != NamedThread::Any)
                return {};

            // Validate prerequisites before the node becomes visible so a rejected
            // submission cannot leave a half-linked node in the graph.
            std::vector<std::shared_ptr<Impl::Node>> prerequisiteNodes;
            prerequisiteNodes.reserve(desc.prerequisites.size());
            for (size_t index = 0u; index < desc.prerequisites.size(); ++index)
            {
                const TaskHandle prerequisite = desc.prerequisites[index];
                if (!prerequisite.IsValid())
                    return {};

                const auto found = m_impl->nodes.find(prerequisite.id);
                if (found == m_impl->nodes.end() ||
                    found->second == nullptr ||
                    found->second->handle.generation != prerequisite.generation)
                {
                    return {};
                }

                for (size_t previous = 0u; previous < index; ++previous)
                {
                    if (desc.prerequisites[previous] == prerequisite)
                        return {};
                }

                prerequisiteNodes.push_back(found->second);
            }

            node = std::make_shared<Impl::Node>();
            node->handle.id = m_impl->nextTaskId.fetch_add(1u, std::memory_order_relaxed);
            node->handle.generation = m_impl->nextGeneration;
            node->desc = std::move(desc);
            node->remainingDependencies = prerequisiteNodes.size();
            node->completion = std::make_shared<TaskCompletionState>();
            node->completion->graph = m_impl;

            for (const auto& prerequisite : prerequisiteNodes)
            {
                if (Impl::IsTerminal(prerequisite->status))
                {
                    if (prerequisite->status == TaskStatus::Succeeded)
                        --node->remainingDependencies;
                    else
                        dependencyAlreadyFailed = true;
                    continue;
                }
                prerequisite->dependents.push_back(node);
            }

            handle = node->handle;
            m_impl->nodes.emplace(handle.id, node);

            if (!dependencyAlreadyFailed && node->remainingDependencies == 0u)
            {
                if (m_impl->config.forceInlineExecution)
                    runInline = true;
                else
                    m_impl->EnqueueReadyLocked(node);
            }
        }

        // Finalize and inline execution both take the graph lock internally, so
        // they must run after the submission critical section is released.
        if (dependencyAlreadyFailed)
        {
            // A failed or cancelled dependency cannot become runnable again.
            m_impl->Finalize(node, TaskStatus::Cancelled);
            return handle;
        }

        if (runInline)
        {
            uint8_t expected = kLifecycleQueued;
            if (node->lifecycle.compare_exchange_strong(
                    expected,
                    kLifecycleClaimed,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                m_impl->runningTaskCount.fetch_add(1u, std::memory_order_acq_rel);
                m_impl->ExecuteTaskBody(node);
                m_impl->runningTaskCount.fetch_sub(1u, std::memory_order_acq_rel);
            }
        }

        return handle;
    }

    bool TaskGraph::RequestCancel(const TaskHandle handle)
    {
        if (m_impl == nullptr || !handle.IsValid())
            return false;

        std::shared_ptr<Impl::Node> node;
        {
            std::lock_guard lock(m_impl->mutex);
            const auto found = m_impl->nodes.find(handle.id);
            if (found == m_impl->nodes.end() || found->second == nullptr)
                return false;
            if (found->second->handle.generation != handle.generation)
                return false;
            if (Impl::IsTerminal(found->second->status))
                return false;
            node = found->second;
        }

        const uint8_t observed = node->lifecycle.load(std::memory_order_acquire);
        if (observed == kLifecycleTerminal)
            return false;

        node->cancelRequested.store(true, std::memory_order_release);

        if (observed == kLifecycleClaimed)
        {
            // The body is already running and cannot be preempted. The request is
            // recorded for the body and observers; the body reports the final
            // status.
            return true;
        }

        uint8_t expected = kLifecycleQueued;
        if (!node->lifecycle.compare_exchange_strong(
                expected,
                kLifecycleTerminal,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            // Lost the race to a claim: the cancellation stays requested.
            return true;
        }

        m_impl->Finalize(node, TaskStatus::Cancelled);
        return true;
    }

    bool TaskGraph::IsCancellationRequested(const TaskHandle handle) const
    {
        if (m_impl == nullptr || !handle.IsValid())
            return false;

        std::lock_guard lock(m_impl->mutex);
        const auto found = m_impl->nodes.find(handle.id);
        if (found == m_impl->nodes.end() || found->second == nullptr)
            return false;
        if (found->second->handle.generation != handle.generation)
            return false;
        return found->second->cancelRequested.load(std::memory_order_acquire);
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
        if (IsWorkerThread())
        {
            size_t executed = 0u;
            while ((maxTasks == 0u || executed < maxTasks) &&
                m_impl->TryExecuteOneReadyTask(g_workerIdentity.workerIndex))
            {
                ++executed;
            }
            return executed;
        }
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
            if (node == nullptr || !Impl::IsTerminal(node->status))
                continue;
            if (node->lifecycle.load(std::memory_order_acquire) == kLifecycleClaimed)
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
        return m_impl->runningTaskCount.load(std::memory_order_acquire);
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
        return m_impl != nullptr &&
            g_workerIdentity.graph == static_cast<const void*>(m_impl.get());
    }

    size_t TaskGraph::GetWorkerStealCount() const
    {
        if (m_impl == nullptr)
            return 0u;
        return m_impl->stealExecutionCount.load(std::memory_order_relaxed);
    }

    size_t TaskGraph::GetWorkerLocalExecutionCount() const
    {
        if (m_impl == nullptr)
            return 0u;
        return m_impl->localExecutionCount.load(std::memory_order_relaxed);
    }

    size_t TaskGraph::GetWorkerInjectionExecutionCount() const
    {
        if (m_impl == nullptr)
            return 0u;
        return m_impl->injectionExecutionCount.load(std::memory_order_relaxed);
    }
}
