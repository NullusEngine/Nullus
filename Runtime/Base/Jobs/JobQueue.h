#pragma once

#include "Jobs/JobTypes.h"
#include "Jobs/WorkerWakeSignal.h"

#include <condition_variable>
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace NLS::Base::Jobs
{
    class JobQueue
    {
    public:
        JobQueue() = default;
        ~JobQueue();

        bool Start(uint32_t workerCount, size_t maxPendingJobCount = 65536u);
        void Shutdown(JobSystemShutdownMode mode);

        JobHandle ScheduleJob(const JobScheduleDesc& desc);
        bool DispatchJob(const JobScheduleDesc& desc, bool tailContinuation);
        JobHandle ScheduleJobDepends(const JobScheduleDesc& desc, JobHandle dependency);
        JobHandle ScheduleJobs(
            std::vector<JobScheduleDesc> jobs,
            JobHandle dependency,
            JobFunction completeFunction = nullptr,
            void* completeUserData = nullptr,
            JobFunction cancelFunction = nullptr,
            void* cancelUserData = nullptr,
            bool runCancelOnFailureAfterStart = false,
            JobPriority priority = JobPriority::Normal,
            JobSafetyPolicy safetyPolicy = JobSafetyPolicy::MaySyncWait,
            const char* debugName = nullptr,
            JobFunction terminalCleanupFunction = nullptr,
            void* terminalCleanupUserData = nullptr);
        JobHandle ScheduleJobsMultiDepends(
            std::vector<JobScheduleDesc> jobs,
            std::vector<JobHandle> dependencies,
            JobFunction completeFunction = nullptr,
            void* completeUserData = nullptr,
            JobFunction cancelFunction = nullptr,
            void* cancelUserData = nullptr,
            bool runCancelOnFailureAfterStart = false,
            JobPriority priority = JobPriority::Normal,
            JobSafetyPolicy safetyPolicy = JobSafetyPolicy::MaySyncWait,
            const char* debugName = nullptr,
            JobFunction terminalCleanupFunction = nullptr,
            void* terminalCleanupUserData = nullptr);
        JobHandle CreatePendingJob(const JobScheduleDesc& desc);
        JobHandle CreatePendingJobs(
            std::vector<JobScheduleDesc> jobs,
            JobHandle dependency,
            JobFunction completeFunction = nullptr,
            void* completeUserData = nullptr,
            JobFunction cancelFunction = nullptr,
            void* cancelUserData = nullptr,
            bool runCancelOnFailureAfterStart = false,
            JobPriority priority = JobPriority::Normal,
            JobSafetyPolicy safetyPolicy = JobSafetyPolicy::MaySyncWait,
            const char* debugName = nullptr,
            JobFunction terminalCleanupFunction = nullptr,
            void* terminalCleanupUserData = nullptr);
        void KickPending(JobHandle handle);

        bool ExecuteOneJob();
        bool ExecuteOneJobForWait(
            JobHandle waitedHandle,
            const std::vector<JobHandle>& waitEligibleHandles = {},
            bool allowNoSyncWaitOpportunisticWork = true);
        void Complete(JobHandle handle);
        bool IsCompleted(JobHandle handle) const;
        bool RequestCancel(JobHandle handle);
        bool IsCancellationRequested(JobHandle handle) const;
        JobCompletionStatus GetCompletionStatus(JobHandle handle) const;
        bool IsKnownHandle(JobHandle handle) const;
        std::vector<JobHandle> CollectDirectDependencies(JobHandle handle) const;
        void ClearWithoutSync(JobHandle handle);
        void StopAcceptingWork();
        void KickAllPending();
        void NotifyExternalDependencyChanged();

        uint32_t GetWorkerCount() const;
        uint64_t GetAcceptedWorkBoundary() const;
        bool IsAcceptedWorkComplete(uint64_t boundary) const;
        bool IsWorkerThread() const;
#if defined(NLS_ENABLE_TEST_HOOKS)
        size_t GetWaitingWorkerCountForTesting() { return m_workAvailable.GetWaitingCountForTesting(); }
        Internal::DispatchReuseStats GetDispatchReuseStatsForTesting() const
        {
            std::lock_guard lock(m_mutex);
            return {m_dispatchAllocations, m_dispatchReuses, m_dispatchCacheSize};
        }
#endif
        size_t GetWorkerStealCount() const { return m_workerStealCount.load(std::memory_order_relaxed); }
        size_t GetWorkerLocalExecutionCount() const { return m_workerLocalExecutionCount.load(std::memory_order_relaxed); }
        size_t GetWorkerInjectionExecutionCount() const { return m_workerInjectionExecutionCount.load(std::memory_order_relaxed); }

    private:
        enum class GroupState : uint8_t
        {
            WaitingForDependencies = 0,
            Queued,
            Running,
            Completed,
            Cancelled,
            Failed,
            PendingKick
        };

        struct JobGroup
        {
            uint64_t id = 0u;
            uint32_t generation = 1u;
            JobPriority priority = JobPriority::Normal;
            JobSafetyPolicy safetyPolicy = JobSafetyPolicy::MaySyncWait;
            std::string debugName;
            JobHandle dependency;
            std::vector<JobHandle> dependencies;
            std::vector<uint64_t> dependents;
            std::vector<JobScheduleDesc> jobs;
            std::vector<std::string> jobDebugNames;
            std::vector<JobSafetyPolicy> jobSafetyPolicies;
            JobFunction completeFunction = nullptr;
            void* completeUserData = nullptr;
            JobFunction cancelFunction = nullptr;
            void* cancelUserData = nullptr;
            JobFunction terminalCleanupFunction = nullptr;
            void* terminalCleanupUserData = nullptr;
            bool runCancelOnFailureAfterStart = false;
            bool cleanupConsumed = false;
            bool cancellationRequested = false;
            bool hasExternalDependency = false;
            bool ownerTracked = false;
            bool terminalCleanupConsumed = false;
            bool completeCallbackRunning = false;
            bool retirementQueued = false;
            bool queuedDiagnosticRecorded = false;
            bool runningDiagnosticRecorded = false;
            size_t nextJobIndex = 0u;
            size_t runningJobCount = 0u;
            size_t completedJobCount = 0u;
            GroupState state = GroupState::WaitingForDependencies;
        };

        using GroupPtr = std::shared_ptr<JobGroup>;
        using GroupMap = std::unordered_map<uint64_t, GroupPtr>;

        GroupMap::node_type TakeReusableDispatchLocked();
        void RecycleCompletedDispatchLocked(const GroupPtr& group);

        GroupPtr FindGroupLocked(JobHandle handle) const;
        static void AppendGroupTerminalCleanup(
            const GroupPtr& group,
            std::vector<std::pair<JobFunction, void*>>& cleanupActions);
        static void OwnJobDebugNames(const GroupPtr& group);
        static void AppendUnstartedJobCancelCallbacks(
            const GroupPtr& group,
            std::vector<std::pair<JobFunction, void*>>& cleanupActions);
        bool IsCompletedLocked(JobHandle handle) const;
        JobCompletionStatus GetDependencyStatusLocked(JobHandle dependency) const;
        bool IsExternalDependencyLocked(JobHandle dependency) const;
        bool IsGroupFinishedLocked(const GroupPtr& group) const;
        bool HasUnfinishedGroupsLocked() const;
        JobCompletionStatus GetGroupCompletionStatusLocked(const GroupPtr& group) const;
        JobCompletionStatus GetGroupDependenciesStatusLocked(const GroupPtr& group) const;
        JobCompletionStatus GetGroupDependenciesStatusLocked(
            const GroupPtr& group,
            const std::vector<std::pair<JobHandle, JobCompletionStatus>>& resolvedExternalDependencies) const;
        void RegisterLocalDependentsLocked(const GroupPtr& group);
        bool IsRetiredHandleLocked(JobHandle handle) const;
        JobCompletionStatus GetRetiredStatusLocked(JobHandle handle) const;
        bool RetireGroupLocked(const GroupPtr& group);
        static void QueueGroupForRetirementLocked(
            const GroupPtr& group,
            std::vector<GroupPtr>& groupsToRetire);
        void RetireQueuedGroupsAfterCallbacks(
            const std::vector<GroupPtr>& groupsToRetire,
            bool& notifyCrossQueueDependency);
        void ReleaseExternalDependenciesForGroupLocked(const GroupPtr& group) const;
        void PruneRetiredHandlesLocked();
        std::pair<JobFunction, void*> CancelGroupLocked(
            const GroupPtr& group,
            std::vector<std::pair<JobFunction, void*>>& cleanupActions);
        void KickPendingGroupLocked(
            const GroupPtr& group,
            bool externalDependency,
            JobCompletionStatus externalDependencyStatus,
            std::vector<std::pair<JobFunction, void*>>& cleanupActions,
            std::vector<GroupPtr>& groupsToRetire,
            bool& notifyCrossQueueDependency);
        bool EnqueueReadyGroupLocked(
            const GroupPtr& group,
            std::vector<std::pair<JobFunction, void*>>& cleanupActions,
            std::vector<GroupPtr>& groupsToRetire,
            bool& notifyCrossQueueDependency);
        void NotifyExternalDependencyChangedLocked();
        void WakeExternalDependencyReadyGroups();
        void WakeExternalDependencyReadyGroupsForWait(JobHandle waitedHandle);
        void WakeExternalDependencyReadyGroupsForHandles(
            const std::vector<JobHandle>* foregroundHandles,
            uint64_t cleanupDiagnosticGroupId);
        void CollectExternalDependenciesForGroupLocked(
            const GroupPtr& group,
            std::vector<JobHandle>& dependencies) const;
        static JobCompletionStatus FindResolvedDependencyStatus(
            JobHandle dependency,
            const std::vector<std::pair<JobHandle, JobCompletionStatus>>& resolvedExternalDependencies);
        bool TryResolveWaitingGroupLocked(
            const GroupPtr& group,
            std::vector<std::pair<JobFunction, void*>>& cleanupActions,
            std::vector<GroupPtr>& groupsToRetire,
            bool& notifyCrossQueueDependency);
        bool TryResolveWaitingGroupLocked(
            const GroupPtr& group,
            const std::vector<std::pair<JobHandle, JobCompletionStatus>>& resolvedExternalDependencies,
            std::vector<std::pair<JobFunction, void*>>& cleanupActions,
            std::vector<GroupPtr>& groupsToRetire,
            bool& notifyCrossQueueDependency);
        std::vector<JobHandle> CollectGroupDependencies(JobHandle handle) const;
        bool HasExternalDependencyWaiterLocked() const;
        struct ReadyQueue
        {
            std::mutex mutex;
            std::atomic<size_t> size{0u};
            std::array<std::deque<uint64_t>, 3u> priorities;
        };
        uint64_t TakeReadyGroup();
        void PushReadyGroupLocked(const GroupPtr& group);
        bool CanAcceptJobsLocked(size_t jobCount) const;
        struct JobExecution
        {
            GroupPtr group;
            JobScheduleDesc job;
            size_t jobIndex = 0u;
            bool recordRunningDiagnostic = false;
            std::vector<std::pair<JobFunction, void*>> cleanupActions;
        };
        std::optional<JobExecution> PopReadyJob();
        std::optional<JobExecution> PopReadyJobForWaitLocked(
            JobHandle waitedHandle,
            const std::vector<JobHandle>& waitEligibleHandles,
            bool allowNoSyncWaitOpportunisticWork);
        void CollectWaitEligibleHandlesLocked(
            JobHandle handle,
            std::vector<JobHandle>& foregroundHandles,
            std::vector<JobHandle>& externalHandles) const;
        void CollectWaitEligibleHandles(
            JobHandle waitedHandle,
            std::vector<JobHandle>& foregroundHandles,
            std::vector<JobHandle>& externalHandles) const;
        std::vector<JobHandle> CollectWaitEligibleForegroundHandles(JobHandle waitedHandle) const;
        std::vector<JobHandle> CollectWaitEligibleExternalHandles(JobHandle waitedHandle) const;
        void ExecuteJob(const JobExecution& execution);
        void ExecuteOwnerTrackedJob(const JobExecution& execution);
        void TryCompleteGroupAfterJob(const GroupPtr& group, bool failed);
        void MarkGroupCompleted(const GroupPtr& group);
        void RunCancelFunction(const GroupPtr& group);
        void WakeDependentsForFinishedGroupLocked(
            const GroupPtr& group,
            std::vector<GroupPtr>& readyDependents,
            std::vector<std::pair<JobFunction, void*>>& cleanupActions,
            std::vector<GroupPtr>& groupsToRetire,
            bool& notifyCrossQueueDependency);
        void CompleteDependencyOnlyGroupLocked(
            const GroupPtr& group,
            std::vector<std::pair<JobFunction, void*>>& cleanupActions,
            std::vector<GroupPtr>& groupsToRetire,
            bool& notifyCrossQueueDependency);
        void WorkerLoop(uint32_t workerIndex);
        std::vector<std::pair<JobFunction, void*>> CancelPendingLocked(
            std::vector<GroupPtr>& groupsToRetire,
            bool& notifyCrossQueueDependency);

        mutable std::mutex m_mutex;
        Internal::WorkerWakeSignal m_workAvailable;
        // Completion observers must not wake every idle worker on each tiny job.
        std::condition_variable m_completionAvailable;
        GroupMap m_groups;
        std::unordered_map<uint64_t, uint32_t> m_retiredGenerations;
        std::unordered_map<uint64_t, JobCompletionStatus> m_retiredStatuses;
        std::deque<uint64_t> m_retiredOrder;
        ReadyQueue m_injectionQueue;
        std::vector<std::unique_ptr<ReadyQueue>> m_workerQueues;
        std::atomic<size_t> m_readyCount{0u};
        std::array<std::atomic<size_t>, 3u> m_readyPriorityCounts{};
        std::atomic<size_t> m_workerStealCount{0u};
        std::atomic<size_t> m_workerLocalExecutionCount{0u};
        std::atomic<size_t> m_workerInjectionExecutionCount{0u};
        size_t m_pendingJobCount = 0u;
        size_t m_externalDependencyGroupCount = 0u;
        size_t m_maxPendingJobCount = 65536u;
        std::vector<std::thread> m_workers;
        uint64_t m_nextGroupId = 1u;
        uint32_t m_queueGeneration = 1u;
        bool m_acceptingWork = false;
        bool m_shutdownRequested = false;
        bool m_externalDependencyChanged = false;

        // Keep the bounded completed-record cache after the normal queue state.
        // Reuse requires the map node to own the group's sole strong reference.
        std::array<GroupMap::node_type, 64u> m_dispatchCache;
        size_t m_dispatchCacheSize = 0u;
#if defined(NLS_ENABLE_TEST_HOOKS)
        size_t m_dispatchAllocations = 0u;
        size_t m_dispatchReuses = 0u;
#endif
    };
}
