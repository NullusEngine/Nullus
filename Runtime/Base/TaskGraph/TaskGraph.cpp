#include "TaskGraph/TaskGraph.h"

#include "Profiling/Profiler.h"
#include "Jobs/JobSystem.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace NLS::Base::TaskGraph
{
    namespace
    {
        constexpr uint8_t kLifecycleQueued = 0u;
        constexpr uint8_t kLifecycleClaimed = 1u;
        constexpr uint8_t kLifecycleTerminal = 2u;
        std::atomic<uint32_t> g_nextGraphGeneration {1u};

        bool IsNamedThread(const NamedThread thread)
        {
            return thread >= NamedThread::Game && thread < NamedThread::Count;
        }

        struct ExecutionScope;
        thread_local ExecutionScope* g_execution = nullptr;
        struct ExecutionScope final
        {
            const void* graph;
            uint64_t taskId;
            ExecutionScope* previous = g_execution;
            ExecutionScope(const void* owner, uint64_t id) : graph(owner), taskId(id)
            { g_execution = this; }
            ~ExecutionScope() { g_execution = previous; }
        };
    }

    struct TaskCompletionState final
    {
        mutable std::mutex mutex;
        std::condition_variable condition;
        TaskStatus status = TaskStatus::Pending;
        std::weak_ptr<TaskGraph::Impl> graph;
        TaskHandle handle;

        [[nodiscard]] bool IsCompleteLocked() const noexcept
        {
            return status != TaskStatus::Pending;
        }

        [[nodiscard]] TaskStatus WaitFor(std::chrono::milliseconds timeout);
    };

    struct TaskGraph::Impl final : std::enable_shared_from_this<TaskGraph::Impl>
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
            // Accepted Jobs callbacks retain their payload in the existing node,
            // avoiding a separate heap allocation per graph dispatch.
            std::shared_ptr<Impl> executionOwner;
            std::shared_ptr<Node> executionNode;
        };

        explicit Impl(TaskGraphConfig inConfig)
            : config(std::move(inConfig))
        {
        }

        ~Impl()
        {
            // ReleaseJobSystem supports the final lease returning from a worker:
            // its shutdown join owner waits for that worker to finish. Accepted
            // dispatches retain Impl until their bodies and finalization return.
            if (schedulerAcquired.exchange(false))
                Jobs::Internal::ReleaseJobSystem();
        }

        TaskGraphConfig config;
        std::atomic<uint32_t> workerCount {0u};
        std::atomic_bool schedulerAcquired {false};
        mutable std::mutex mutex;
        std::mutex lifecycleMutex;
        // Guards the scheduler lease acquisition only. It is never held while
        // the graph waits for tasks, so a task body can restore the lease.
        std::mutex schedulerMutex;
        std::condition_variable changed;
        size_t inFlightDispatchCount = 0u;
        size_t finalizingTaskCount = 0u;
        size_t pendingTaskCount = 0u;
        std::array<std::deque<std::shared_ptr<Node>>, static_cast<size_t>(NamedThread::Count)> namedQueues;
        std::array<TaskEvent, static_cast<size_t>(NamedThread::Count)> namedEvents;
        std::array<std::unordered_set<std::thread::id>, static_cast<size_t>(NamedThread::Count)> namedThreadIds;
        std::unordered_map<uint64_t, std::shared_ptr<Node>> nodes;
        // Terminal nodes in finalization order. The front entries are recycled once
        // the configured retention bound is exceeded.
        std::deque<TaskHandle> terminalNodeOrder;
        std::atomic<uint64_t> nextTaskId { 1u };
        std::atomic<size_t> runningTaskCount { 0u };
        size_t initialLocalCount = 0u;
        size_t initialStealCount = 0u;
        size_t initialInjectionCount = 0u;
        uint32_t nextGeneration = 1u;
        bool started = false;
        bool acceptingWork = true;
        bool shuttingDown = false;

        static bool IsTerminal(const TaskStatus status)
        {
            return status != TaskStatus::Pending;
        }

        /**
         * Makes sure this graph owns a scheduler lease on a running pool.
         *
         * The shared pool can be released or reset while a graph stays started,
         * for example when a test resets the scheduler. A graph without a live
         * lease has no executor, so its tasks would never run. The lease is
         * acquired at most once: a graph that already holds one keeps it.
         */
        bool EnsureSchedulerLease()
        {
            if (Jobs::Internal::GetSchedulerLeaseCount() != 0u)
                return true;
            std::lock_guard leaseLock(schedulerMutex);
            if (Jobs::Internal::GetSchedulerLeaseCount() != 0u)
                return true;
            return AcquireSchedulerLease();
        }

        /** Takes a new lease on the shared pool for this graph. */
        bool AcquireSchedulerLease()
        {
            Jobs::JobSystemConfig jobs;
            jobs.workerCount = (std::max)(1u, config.workerCount);
            // Job diagnostics are an application-level Jobs setting. The legacy
            // graph flag must not silently enable global per-job tracing.
            jobs.enableDiagnostics = false;
            if (!Jobs::Internal::AcquireJobSystem(jobs))
                return false;
            schedulerAcquired.store(true, std::memory_order_release);
            workerCount.store(Jobs::GetJobWorkerCount(), std::memory_order_release);
            return true;
        }

        /**
         * Drops a finished node's captured payload. A node whose body is still
         * executing on another thread must keep its std::function alive, so the
         * release is only allowed for the thread that ran the body or for a node
         * that was never claimed.
         */
        static void ReleaseNodePayload(
            const std::shared_ptr<Node>& node,
            const bool bodyFinishedOnThisThread,
            std::pmr::vector<std::function<void()>>& retiredPayloads)
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
            // The body has returned (or never started). Move its closure to a
            // caller-owned list so capture destructors execute outside mutex.
            if (node->desc.function)
                retiredPayloads.push_back(std::move(node->desc.function));
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

        void FinishDispatch()
        {
            std::lock_guard lock(mutex);
            --inFlightDispatchCount;
            changed.notify_all();
        }

        void ExecuteNode(const std::shared_ptr<Node>& node)
        {
            uint8_t expected = kLifecycleQueued;
            if (!node->lifecycle.compare_exchange_strong(expected, kLifecycleClaimed,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return;
            runningTaskCount.fetch_add(1u, std::memory_order_acq_rel);
            ExecuteTaskBody(node);
            runningTaskCount.fetch_sub(1u, std::memory_order_acq_rel);
            changed.notify_all();
        }

        // Never submit to Jobs while holding the graph mutex: cancellation or a
        // fast worker may call back into Finalize before ScheduleJob returns.
        struct DispatchScope final
        {
            std::shared_ptr<Impl> graph;
            std::shared_ptr<Node> node;
            explicit DispatchScope(Node* raw)
                : graph(std::move(raw->executionOwner)), node(std::move(raw->executionNode)) {}
            ~DispatchScope() { graph->FinishDispatch(); }
        };

        void EnqueueReady(const std::shared_ptr<Node>& node, const bool tailContinuation = false)
        {
            {
                std::lock_guard lock(mutex);
                if (node == nullptr || IsTerminal(node->status))
                    return;
                if (node->desc.targetThread != NamedThread::Any)
                {
                    const auto index = static_cast<size_t>(node->desc.targetThread);
                    namedQueues[index].push_back(node);
                    namedEvents[index].Trigger();
                    changed.notify_all();
                    return;
                }
                // No allocation after counting an accepted dispatch. Ownership
                // transfers exactly once, either to a callback or rejection cleanup.
                node->executionOwner = shared_from_this();
                node->executionNode = node;
                ++inFlightDispatchCount;
            }

            Jobs::JobScheduleDesc job;
            job.function = [](void* data)
            {
                DispatchScope dispatch(static_cast<Node*>(data));
                dispatch.graph->ExecuteNode(dispatch.node);
            };
            job.cancelFunction = [](void* data)
            {
                DispatchScope dispatch(static_cast<Node*>(data));
                dispatch.graph->Finalize(dispatch.node, TaskStatus::Cancelled);
            };
            job.userData = node.get();
            job.cancelUserData = node.get();
            job.priority = node->desc.priority == TaskPriority::High ? Jobs::JobPriority::High
                : node->desc.priority == TaskPriority::Low ? Jobs::JobPriority::Low
                : Jobs::JobPriority::Normal;
            job.debugName = "TaskGraph::Dispatch";
            try
            {
                if (Jobs::Internal::DispatchJob(job, tailContinuation))
                    return;
            }
            catch (...)
            {
                DispatchScope dispatch(node.get());
                Finalize(node, TaskStatus::Failed);
                return;
            }
            // A rejected dispatch means the shared pool is gone. The graph can
            // still be started, so restore the pool and retry before the task
            // is cancelled.
            if (EnsureSchedulerLease() && Jobs::Internal::DispatchJob(job, tailContinuation))
                return;
            // A saturated or stopped scheduler never takes ownership on rejection.
            DispatchScope dispatch(node.get());
            Finalize(node, TaskStatus::Cancelled);
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

        struct FinalizationScope final
        {
            Impl& graph;
            bool active = false;
            explicit FinalizationScope(Impl& owner) : graph(owner) {}
            FinalizationScope(const FinalizationScope&) = delete;
            FinalizationScope& operator=(const FinalizationScope&) = delete;
            FinalizationScope(FinalizationScope&&) = delete;
            FinalizationScope& operator=(FinalizationScope&&) = delete;
            void BeginLocked() noexcept
            {
                ++graph.finalizingTaskCount;
                active = true;
            }
            ~FinalizationScope()
            {
                if (!active)
                    return;
                std::lock_guard lock(graph.mutex);
                --graph.finalizingTaskCount;
                graph.changed.notify_all();
            }
        };

        void FinalizeSingleSuccess(const std::shared_ptr<Node>& root,
            std::unique_lock<std::mutex> lock, FinalizationScope& finalization)
        {
            std::shared_ptr<Node> successor;
            std::function<void()> retiredPayload;
            bool finalized = false;
            {
                // This inner lock is destroyed before payload/strong references
                // on every exit, including allocation failure while recording history.
                std::unique_lock critical(std::move(lock));
                finalization.BeginLocked();
                if (!IsTerminal(root->status))
                {
                    // Reserve the only potentially allocating record before
                    // changing lifecycle state or moving a user-owned closure.
                    terminalNodeOrder.push_back(root->handle);
                    root->status = TaskStatus::Succeeded;
                    --pendingTaskCount;
                    root->lifecycle.exchange(kLifecycleTerminal, std::memory_order_acq_rel);
                    if (!root->dependents.empty())
                    {
                        successor = root->dependents.front().lock();
                        if (successor != nullptr && !IsTerminal(successor->status))
                        {
                            if (successor->remainingDependencies > 0u)
                                --successor->remainingDependencies;
                            if (successor->remainingDependencies != 0u)
                                successor.reset();
                        }
                        else
                            successor.reset();
                    }
                    root->dependents.clear();
                    root->desc.prerequisites.clear();
                    retiredPayload = std::move(root->desc.function);
                    PruneTerminalNodesLocked();
                    finalized = true;
                }
            }
            if (finalized)
            {
                retiredPayload = {};
                SetCompletionStatus(root, TaskStatus::Succeeded);
                if (successor != nullptr)
                    EnqueueReady(successor, true);
            }
            changed.notify_all();
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

            // Cancellation may destroy the last facade reference from a closure.
            // Treat its cleanup as execution so facade teardown cannot join this
            // same finalization. Successful dispatch already has ExecutionScope.
            std::optional<ExecutionScope> cleanupExecution;
            if (status != TaskStatus::Succeeded)
                cleanupExecution.emplace(this, root->handle.id);

            // Outlive all payloads and temporary node references, including
            // reentrant user destruction on either finalization path.
            FinalizationScope finalization {*this};
            std::unique_lock<std::mutex> candidateLock(mutex, std::defer_lock);
            if (status == TaskStatus::Succeeded)
            {
                candidateLock.lock();
                if (root->desc.targetThread == NamedThread::Any && root->dependents.size() <= 1u)
                {
                    FinalizeSingleSuccess(root, std::move(candidateLock), finalization);
                    return;
                }
            }

            // A successful node with one successor needs only a few records.
            // Keep those on this call's stack; branching/cancellation may spill
            // to the standard allocator. Reentrant capture destructors get their
            // own storage, so no thread-local scratch ownership escapes the call.
            std::array<std::byte, 256u> scratch;
            std::pmr::monotonic_buffer_resource scratchResource(scratch.data(), scratch.size());
            std::pmr::vector<std::shared_ptr<Node>> readyNodes(&scratchResource);
            std::pmr::vector<std::pair<std::shared_ptr<Node>, TaskStatus>> terminalNodes(&scratchResource);
            std::pmr::vector<std::function<void()>> retiredPayloads(&scratchResource);
            {
                // A fast-path miss hands off the already-held lock. Place the
                // owner after the containers so exception unwinding unlocks
                // before destroying any captured user objects.
                std::unique_lock lock(std::move(candidateLock));
                if (!lock.owns_lock())
                    lock.lock();
                finalization.BeginLocked();
                std::array<bool, static_cast<size_t>(NamedThread::Count)> pruneNamed {};

                std::pmr::vector<std::pair<std::shared_ptr<Node>, TaskStatus>> pending(&scratchResource);
                pending.emplace_back(root, status);

                while (!pending.empty())
                {
                    auto [node, nodeStatus] = std::move(pending.back());
                    pending.pop_back();

                    if (node == nullptr || IsTerminal(node->status))
                        continue;

                    node->status = nodeStatus;
                    --pendingTaskCount;
                    if (node->desc.targetThread != NamedThread::Any)
                        pruneNamed[static_cast<size_t>(node->desc.targetThread)] = true;
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
                        node == root || previousLifecycle != kLifecycleClaimed,
                        retiredPayloads);
                    terminalNodeOrder.push_back(node->handle);
                    PruneTerminalNodesLocked();
                }
                // Cancelled named tasks have no Jobs dispatch to retire them.
                // Remove their queue references so repeated submit/cancel cannot
                // retain unbounded payloads or queue entries without a lane pump.
                for (size_t index = 0; index < pruneNamed.size(); ++index)
                    if (pruneNamed[index])
                        std::erase_if(namedQueues[index], [](const auto& node)
                        { return node == nullptr || IsTerminal(node->status); });
            }

            // Destroy the moved closures outside the graph lock before marking
            // completion observable. No running body is included in this list.
            retiredPayloads.clear();
            // Publish terminal state for every node this call finalized, including
            // the transitively cancelled dependents. A waiter that observed the
            // node before this point would otherwise never be released.
            for (const auto& [node, nodeStatus] : terminalNodes)
                SetCompletionStatus(node, nodeStatus);
            for (const auto& ready : readyNodes)
                EnqueueReady(ready, root->desc.targetThread == NamedThread::Any);
            changed.notify_all();
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
            ExecutionScope execution(this, node->handle.id);
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

        void ClearQueuesLocked()
        {
            for (auto& queue : namedQueues)
                queue.clear();
            for (auto& event : namedEvents)
                event.Reset();
        }

        void CancelPending(const bool orphanedNamedOnly)
        {
            std::vector<std::shared_ptr<Node>> cancelled;
            {
                std::lock_guard lock(mutex);
                for (const auto& [id, node] : nodes)
                {
                    if (IsTerminal(node->status))
                        continue;
                    if (orphanedNamedOnly &&
                        (node->desc.targetThread == NamedThread::Any ||
                         !namedThreadIds[static_cast<size_t>(node->desc.targetThread)].empty()))
                        continue;
                    uint8_t expected = kLifecycleQueued;
                    if (node->lifecycle.compare_exchange_strong(expected, kLifecycleTerminal,
                            std::memory_order_acq_rel, std::memory_order_acquire))
                        cancelled.push_back(node);
                }
            }
            // Finalize may prune the task table; never iterate that table while pruning.
            for (const auto& node : cancelled)
                Finalize(node, TaskStatus::Cancelled);
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
                    const auto& owners = namedThreadIds[static_cast<size_t>(thread)];
                    if (owners.find(std::this_thread::get_id()) == owners.end())
                        return executedCount;
                    node = PopNamedNodeLocked(thread);
                }

                if (node == nullptr)
                    break;

                ExecuteNode(node);
                ++executedCount;
            }

            {
                std::lock_guard lock(mutex);
                if (namedQueues[static_cast<size_t>(thread)].empty())
                    namedEvents[static_cast<size_t>(thread)].Reset();
            }
            return executedCount;
        }

        size_t PumpOwnedNamedThreads(const size_t maxTasks)
        {
            size_t executed = 0u;
            while (true)
            {
                const auto before = executed;
                for (const auto lane : {NamedThread::Game, NamedThread::Render, NamedThread::Rhi})
                {
                    // PumpNamedThread checks registration on each visit. A thread
                    // may own several lanes when dedicated rendering is disabled.
                    executed += PumpNamedThread(lane, 1u);
                    if (maxTasks != 0u && executed >= maxTasks)
                        return executed;
                }
                if (executed == before)
                    return executed;
            }
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

                if (!infiniteWait && std::chrono::steady_clock::now() >= deadline)
                    return TaskStatus::Pending;

                // A blocked pool worker cooperates instead of parking, so a nested
                // wait cannot deadlock even when the graph owns a single worker.
                if (g_execution != nullptr && WaitWouldCycle(state.handle))
                    throw std::logic_error("TaskGraph wait depends on the current execution stack");
                // Visit every owned lane once: choosing only the first registered
                // lane strands Render/Rhi continuations on a combined owner.
                bool assisted = false;
                for (const auto lane : {NamedThread::Game, NamedThread::Render, NamedThread::Rhi})
                    assisted = PumpNamedThread(lane, 1u) != 0u || assisted;
                if (!assisted)
                    assisted = AssistWhileWaiting();

                std::unique_lock lock(state.mutex);
                if (state.IsCompleteLocked())
                    return state.status;

                if (assisted)
                    continue;

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
            return Jobs::Internal::IsForegroundWorkerThread();
        }

        bool AssistWhileWaiting()
        {
            // Jobs explicitly supports a zero-worker cooperative configuration.
            if (workerCount == 0u)
                return Jobs::ExecuteOneJobQueueJob();
            return Jobs::Internal::ExecuteOneForegroundJobForWorker();
        }

        bool WaitWouldCycle(const TaskHandle target)
        {
            std::lock_guard lock(mutex);
            std::vector<uint64_t> pending {target.id};
            std::unordered_set<uint64_t> visited;
            while (!pending.empty())
            {
                const auto id = pending.back();
                pending.pop_back();
                if (!visited.insert(id).second)
                    continue;
                for (auto* scope = g_execution; scope; scope = scope->previous)
                    if (scope->graph == this && scope->taskId == id)
                        return true;
                const auto found = nodes.find(id);
                if (found == nodes.end() || IsTerminal(found->second->status))
                    continue;
                for (const auto prerequisite : found->second->desc.prerequisites)
                    pending.push_back(prerequisite.id);
            }
            return false;
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
        for (auto* scope = g_execution; scope; scope = scope->previous)
        {
            if (scope->graph != m_impl.get())
                continue;
            // A closure may own the facade's last reference. Do not wait on the
            // current callback; cancel unstarted nodes and let dispatch ownership
            // keep the implementation alive until all running callbacks return.
            {
                std::lock_guard lock(m_impl->mutex);
                m_impl->acceptingWork = false;
            }
            m_impl->CancelPending(false);
            return;
        }
        Shutdown(TaskGraphShutdownMode::Immediate);
    }

    bool TaskGraph::Start()
    {
        return StartInternal(true);
    }

    bool TaskGraph::StartInternal(const bool explicitStart)
    {
        if (m_impl == nullptr)
            return false;
        std::lock_guard lifecycleLock(m_impl->lifecycleMutex);
        std::lock_guard lock(m_impl->mutex);
        if (!explicitStart && !m_impl->acceptingWork)
            return false;
        if (m_impl->started)
        {
            // The shared pool can be released or reset while this graph stays
            // started. Without a live lease the graph has no executor and its
            // tasks would never run, so the pool has to come back before the
            // caller is told that the graph is ready.
            return m_impl->EnsureSchedulerLease();
        }
        {
            std::lock_guard leaseLock(m_impl->schedulerMutex);
            if (!m_impl->AcquireSchedulerLease())
                return false;
        }
        m_impl->initialLocalCount = Jobs::Internal::GetJobWorkerLocalExecutionCount();
        m_impl->initialStealCount = Jobs::Internal::GetJobWorkerStealCount();
        m_impl->initialInjectionCount = Jobs::Internal::GetJobWorkerInjectionExecutionCount();
        m_impl->nextGeneration = g_nextGraphGeneration.fetch_add(1u, std::memory_order_relaxed);
        if (m_impl->nextGeneration == 0u)
            m_impl->nextGeneration = g_nextGraphGeneration.fetch_add(1u, std::memory_order_relaxed);
        m_impl->acceptingWork = true;
        m_impl->started = true;
        return true;
    }

    void TaskGraph::Shutdown(const TaskGraphShutdownMode mode)
    {
        if (m_impl == nullptr)
            return;
        // Joining the callback's own lifetime is impossible. Keep this operation
        // on the lifecycle owner; task bodies can request cancellation instead.
        for (auto* scope = g_execution; scope; scope = scope->previous)
            if (scope->graph == m_impl.get())
                return;
        std::lock_guard lifecycleLock(m_impl->lifecycleMutex);
        {
            std::lock_guard lock(m_impl->mutex);
            m_impl->acceptingWork = false;
            if (!m_impl->started)
                return;
            m_impl->shuttingDown = true;
        }

        while (true)
        {
            // Unregistered lanes have no executor. Cancel their unstarted work
            // instead of running graphics/main-thread code on the shutdown caller.
            m_impl->CancelPending(mode == TaskGraphShutdownMode::DrainAcceptedWork);
            if (mode == TaskGraphShutdownMode::DrainAcceptedWork)
            {
                for (const auto lane : {NamedThread::Game, NamedThread::Render, NamedThread::Rhi})
                    m_impl->PumpNamedThread(lane, 1u);
            }
            const bool assisted = m_impl->AssistWhileWaiting();
            std::unique_lock lock(m_impl->mutex);
            if (m_impl->pendingTaskCount == 0u && m_impl->inFlightDispatchCount == 0u &&
                m_impl->finalizingTaskCount == 0u &&
                m_impl->runningTaskCount.load(std::memory_order_acquire) == 0u)
                break;
            if (!assisted)
                m_impl->changed.wait_for(lock, std::chrono::milliseconds(1));
        }

        {
            std::lock_guard lock(m_impl->mutex);
            m_impl->nodes.clear();
            m_impl->terminalNodeOrder.clear();
            m_impl->ClearQueuesLocked();
            m_impl->started = false;
            m_impl->shuttingDown = false;
            m_impl->workerCount = 0u;
        }
        if (m_impl->schedulerAcquired.exchange(false))
            Jobs::Internal::ReleaseJobSystem();
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
        // Logical RHI Tasks mode explicitly registers pool workers. Main and
        // Render are dedicated lanes and must not migrate into the compute pool.
        if (thread != NamedThread::Rhi && Jobs::Internal::IsForegroundWorkerThread())
            return false;
        if (thread != NamedThread::Rhi && !m_impl->namedThreadIds[index].empty() &&
            !m_impl->namedThreadIds[index].contains(current))
            return false;
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
        return SubmitInternal(std::move(desc), nullptr);
    }

    TaskSubmission TaskGraph::SubmitTracked(TaskDesc desc)
    {
        TaskSubmission result;
        result.handle = SubmitInternal(std::move(desc), &result.completion);
        return result;
    }

    TaskGraphBatch::NodeId TaskGraphBatch::Add(TaskDesc task)
    {
        if (m_executed || !task.function || !task.prerequisites.empty())
            return InvalidNode;
        m_tasks.push_back(std::move(task));
        m_dependencies.emplace_back();
        return m_tasks.size() - 1u;
    }

    bool TaskGraphBatch::DependsOn(const NodeId node, const NodeId prerequisite)
    {
        if (m_executed || node >= m_tasks.size() || prerequisite >= m_tasks.size())
            return false;
        auto& dependencies = m_dependencies[node];
        if (std::find(dependencies.begin(), dependencies.end(), prerequisite) != dependencies.end())
            return false;
        dependencies.push_back(prerequisite);
        return true;
    }

    GraphCompletion TaskGraphBatch::Execute(TaskGraph& graph)
    {
        return graph.SubmitGraph(*this);
    }

    GraphCompletion TaskGraph::SubmitGraph(TaskGraphBatch& batch)
    {
        GraphCompletion result;
        if (batch.m_executed)
            return result;
        batch.m_executed = true;
        const size_t count = batch.m_tasks.size();
        std::vector<size_t> remaining(count);
        std::vector<std::vector<size_t>> dependents(count);
        std::vector<size_t> order;
        order.reserve(count);
        for (size_t index = 0; index < count; ++index)
        {
            const auto& task = batch.m_tasks[index];
            if (!task.function || (!IsNamedThread(task.targetThread) && task.targetThread != NamedThread::Any) ||
                task.priority > TaskPriority::High || !task.prerequisites.empty())
                return result;
            remaining[index] = batch.m_dependencies[index].size();
            for (const auto predecessor : batch.m_dependencies[index])
                dependents[predecessor].push_back(index);
            if (remaining[index] == 0u)
                order.push_back(index);
        }
        for (size_t cursor = 0; cursor < order.size(); ++cursor)
            for (const auto successor : dependents[order[cursor]])
                if (--remaining[successor] == 0u)
                    order.push_back(successor);
        if (order.size() != count)
            return result; // Includes self edges; no node has been submitted.

        {
            std::lock_guard lock(m_impl->mutex);
            if (!m_impl->acceptingWork || m_impl->shuttingDown)
                return result;
        }
        if (!IsStarted() && !StartInternal(false))
            return result;

        std::vector<std::shared_ptr<Impl::Node>> nodes;
        nodes.reserve(count);
        result.m_tasks.reserve(count);
        {
            std::lock_guard lock(m_impl->mutex);
            if (!m_impl->acceptingWork || m_impl->shuttingDown ||
                count > m_impl->config.maxPendingTaskCount - m_impl->pendingTaskCount)
                return result;
            // Prepare the complete graph before publishing roots. This remains
            // correct with terminal retention set to zero and very fast workers.
            for (size_t index = 0; index < count; ++index)
            {
                auto node = std::make_shared<Impl::Node>();
                node->handle = {m_impl->nextTaskId.fetch_add(1u), m_impl->nextGeneration};
                node->desc = std::move(batch.m_tasks[index]);
                node->remainingDependencies = batch.m_dependencies[index].size();
                node->completion = std::make_shared<TaskCompletionState>();
                node->completion->graph = m_impl;
                node->completion->handle = node->handle;
                nodes.push_back(std::move(node));
            }
            for (size_t index = 0; index < count; ++index)
            {
                for (const auto predecessor : batch.m_dependencies[index])
                {
                    nodes[predecessor]->dependents.push_back(nodes[index]);
                    nodes[index]->desc.prerequisites.push_back(nodes[predecessor]->handle);
                }
                result.m_tasks.push_back({nodes[index]->handle, TaskCompletion(nodes[index]->completion)});
            }
            try
            {
                for (const auto& node : nodes)
                    m_impl->nodes.emplace(node->handle.id, node);
            }
            catch (...)
            {
                for (const auto& node : nodes)
                    m_impl->nodes.erase(node->handle.id);
                throw;
            }
            m_impl->pendingTaskCount += count;
            result.m_valid = true;
        }
        for (const auto index : order)
            if (batch.m_dependencies[index].empty())
                m_impl->EnqueueReady(nodes[index]);
        return result;
    }

    bool GraphCompletion::IsComplete() const
    {
        return m_valid && std::all_of(m_tasks.begin(), m_tasks.end(),
            [](const auto& task) { return task.completion.IsComplete(); });
    }

    TaskStatus GraphCompletion::GetStatus() const
    {
        if (!m_valid)
            return TaskStatus::Cancelled;
        TaskStatus status = TaskStatus::Succeeded;
        for (const auto& task : m_tasks)
        {
            const auto taskStatus = task.completion.GetStatus();
            if (taskStatus == TaskStatus::Pending)
                return TaskStatus::Pending;
            if (taskStatus == TaskStatus::Failed ||
                (taskStatus == TaskStatus::Cancelled && status != TaskStatus::Failed))
                status = taskStatus;
        }
        return status;
    }

    TaskStatus GraphCompletion::Wait()
    {
        for (auto& task : m_tasks)
            (void)task.completion.Wait();
        return GetStatus();
    }

    bool GraphCompletion::WaitFor(const std::chrono::milliseconds timeout)
    {
        if (!m_valid)
            return false;
        if (timeout == std::chrono::milliseconds::max())
        {
            (void)Wait();
            return true;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (auto& task : m_tasks)
        {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (!task.completion.WaitFor((std::max)(remaining, std::chrono::milliseconds(0))))
                return false;
        }
        return true;
    }

    TaskHandle TaskGraph::SubmitInternal(TaskDesc desc, TaskCompletion* completion)
    {
        if (m_impl == nullptr || !desc.function)
            return {};

        std::shared_ptr<Impl::Node> node;
        TaskHandle handle;
        bool dependencyAlreadyFailed = false;
        bool runInline = false;
        bool enqueue = false;
        {
            // The started path validates and admits under one lock. Lazy startup
            // still runs outside this mutex and rechecks admission after relocking.
            // Keep node outside the lock scope so capture cleanup remains reentrant.
            std::unique_lock lock(m_impl->mutex);
            if (!m_impl->acceptingWork || m_impl->shuttingDown)
                return {};
            if (!m_impl->started)
            {
                lock.unlock();
                if (!StartInternal(false))
                    return {};
                lock.lock();
            }
            if (!m_impl->acceptingWork || m_impl->shuttingDown ||
                m_impl->pendingTaskCount >= m_impl->config.maxPendingTaskCount)
                return {};
            if (desc.priority > TaskPriority::High)
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
            node->completion->handle = node->handle;

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
            ++m_impl->pendingTaskCount;

            if (completion != nullptr)
                *completion = TaskCompletion(node->completion);

            if (!dependencyAlreadyFailed && node->remainingDependencies == 0u)
            {
                runInline = m_impl->config.forceInlineExecution &&
                    (node->desc.targetThread == NamedThread::Any ||
                     m_impl->namedThreadIds[static_cast<size_t>(node->desc.targetThread)].contains(
                         std::this_thread::get_id()));
                enqueue = !runInline;
            }
        }

        // Finalize and inline execution both take the graph lock internally, so
        // they must run after the submission critical section is released.
        if (dependencyAlreadyFailed)
        {
            // A failed or cancelled dependency cannot become runnable again.
            const auto impl = m_impl;
            impl->Finalize(node, TaskStatus::Cancelled);
            return handle;
        }

        if (runInline)
        {
            // The closure may own the facade's last reference.
            const auto impl = m_impl;
            impl->ExecuteNode(node);
        }
        else if (enqueue)
            m_impl->EnqueueReady(node);

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
            if (Impl::IsTerminal(found->second->status) || !found->second->desc.cancellable)
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

        const auto impl = m_impl;
        impl->Finalize(node, TaskStatus::Cancelled);
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
        const auto impl = m_impl;
        if (impl == nullptr)
            return 0u;
        NamedThread named;
        {
            std::lock_guard lock(impl->mutex);
            named = impl->GetCurrentNamedThreadLocked();
        }
        if (named != NamedThread::Any)
            return impl->PumpOwnedNamedThreads(maxTasks);
        if (impl->IsCurrentThreadGraphWorker())
        {
            size_t executed = 0u;
            while ((maxTasks == 0u || executed < maxTasks) && impl->AssistWhileWaiting())
                ++executed;
            return executed;
        }
        return 0u;
    }

    size_t TaskGraph::Pump(const NamedThread thread, const size_t maxTasks)
    {
        const auto impl = m_impl;
        if (impl == nullptr || !IsNamedThread(thread))
            return 0u;
        return impl->PumpNamedThread(thread, maxTasks);
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
        auto completion = GetCompletion(handle);
        return completion.IsValid() ? completion.Wait() : TaskStatus::Cancelled;
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
        if (m_impl == nullptr)
            return 0u;
        std::lock_guard lock(m_impl->mutex);
        return m_impl->workerCount;
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
        return m_impl != nullptr && Jobs::Internal::IsForegroundWorkerThread();
    }

    size_t TaskGraph::GetWorkerStealCount() const
    {
        if (m_impl == nullptr)
            return 0u;
        std::lock_guard lock(m_impl->mutex);
        if (!m_impl->started)
            return 0u;
        const auto count = Jobs::Internal::GetJobWorkerStealCount();
        return count - (std::min)(count, m_impl->initialStealCount);
    }

    size_t TaskGraph::GetWorkerLocalExecutionCount() const
    {
        if (m_impl == nullptr)
            return 0u;
        std::lock_guard lock(m_impl->mutex);
        if (!m_impl->started)
            return 0u;
        const auto count = Jobs::Internal::GetJobWorkerLocalExecutionCount();
        return count - (std::min)(count, m_impl->initialLocalCount);
    }

    size_t TaskGraph::GetWorkerInjectionExecutionCount() const
    {
        if (m_impl == nullptr)
            return 0u;
        std::lock_guard lock(m_impl->mutex);
        if (!m_impl->started)
            return 0u;
        const auto count = Jobs::Internal::GetJobWorkerInjectionExecutionCount();
        return count - (std::min)(count, m_impl->initialInjectionCount);
    }
}
