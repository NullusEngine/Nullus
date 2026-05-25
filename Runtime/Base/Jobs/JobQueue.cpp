#include "Jobs/JobQueue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <sstream>

#include "Jobs/JobSystem.h"
#include "Jobs/BackgroundJobQueue.h"
#include "Jobs/JobDiagnostics.h"
#include "Jobs/JobSafety.h"
#include "Profiling/Profiler.h"

namespace NLS::Base::Jobs
{
    namespace
    {
        constexpr size_t kRetiredHandleHistoryLimit = 4096u;
        constexpr uint32_t kMaxHighPriorityDispatchStreak = 8u;
        std::atomic<uint32_t> g_nextQueueGeneration{1u};

        uint32_t AllocateQueueGeneration()
        {
            uint32_t generation = 0u;
            do
            {
                generation = g_nextQueueGeneration.fetch_add(1u, std::memory_order_relaxed);
            } while (generation == 0u);
            return generation;
        }
    }

    JobQueue::~JobQueue()
    {
        Shutdown(JobSystemShutdownMode::Immediate);
    }

    namespace
    {
        void NotifyCrossQueueDependencyIfNeeded(const bool needed)
        {
            if (needed)
                Internal::NotifyBackgroundDependencyChanged();
        }

        void RunUserCallback(
            const JobFunction function,
            void* userData,
            const uint64_t jobId,
            const char* message)
        {
            if (function == nullptr)
                return;
            try
            {
                function(userData);
            }
            catch (...)
            {
                Internal::RecordJobViolation(
                    JobViolationKind::CallbackException,
                    jobId,
                    message);
            }
        }

        class JobWorkerExecutionScope
        {
        public:
            explicit JobWorkerExecutionScope(const JobHandle handle)
            {
                Internal::EnterJobWorkerExecution(handle);
            }

            ~JobWorkerExecutionScope()
            {
                Internal::ExitJobWorkerExecution();
            }

            JobWorkerExecutionScope(const JobWorkerExecutionScope&) = delete;
            JobWorkerExecutionScope& operator=(const JobWorkerExecutionScope&) = delete;
        };
    }

    bool JobQueue::Start(const uint32_t workerCount)
    {
        Shutdown(JobSystemShutdownMode::Immediate);

        {
            std::lock_guard lock(m_mutex);
            m_acceptingWork = true;
            m_shutdownRequested = false;
            m_queueGeneration = AllocateQueueGeneration();
        }

        try
        {
            m_workers.reserve(workerCount);
            for (uint32_t workerIndex = 0u; workerIndex < workerCount; ++workerIndex)
                m_workers.emplace_back([this, workerIndex] { WorkerLoop(workerIndex); });
        }
        catch (...)
        {
            Shutdown(JobSystemShutdownMode::Immediate);
            return false;
        }

        return true;
    }

    void JobQueue::Shutdown(const JobSystemShutdownMode mode)
    {
        if (mode == JobSystemShutdownMode::DrainAcceptedWork)
        {
            while (true)
            {
                bool hasUnfinishedWork = false;
                StopAcceptingWork();
                KickAllPending();

                {
                    std::lock_guard lock(m_mutex);
                    hasUnfinishedWork = HasUnfinishedGroupsLocked();
                }

                if (!hasUnfinishedWork)
                    break;

                WakeExternalDependencyReadyGroups();
                if (!ExecuteOneJob())
                    std::this_thread::yield();
            }
        }

        std::vector<std::thread> workers;
        std::vector<std::pair<JobFunction, void*>> pendingCleanup;
        std::vector<GroupPtr> groupsToRetire;
        bool notifyCrossQueueDependency = false;
        {
            std::lock_guard lock(m_mutex);
            m_acceptingWork = false;
            m_shutdownRequested = true;
            if (mode == JobSystemShutdownMode::Immediate)
                pendingCleanup = CancelPendingLocked(groupsToRetire, notifyCrossQueueDependency);
            workers = std::move(m_workers);
        }
        m_workAvailable.notify_all();
        NotifyCrossQueueDependencyIfNeeded(notifyCrossQueueDependency);

        for (const auto& [cleanupFunction, cleanupUserData] : pendingCleanup)
        {
            RunUserCallback(
                cleanupFunction,
                cleanupUserData,
                0u,
                "Foreground job cleanup callback threw an exception.");
        }
        RetireQueuedGroupsAfterCallbacks(groupsToRetire, notifyCrossQueueDependency);
        NotifyCrossQueueDependencyIfNeeded(notifyCrossQueueDependency);

        for (auto& worker : workers)
        {
            if (worker.joinable())
                worker.join();
        }

        {
            std::unique_lock lock(m_mutex);
            m_workAvailable.wait(
                lock,
                [this]
                {
                    return !HasUnfinishedGroupsLocked();
                });
        }

        {
            std::lock_guard lock(m_mutex);
            m_highPriorityQueue.clear();
            m_normalPriorityQueue.clear();
            m_groups.clear();
            m_retiredGenerations.clear();
            m_retiredStatuses.clear();
            m_retiredOrder.clear();
            m_shutdownRequested = false;
        }
    }

    JobHandle JobQueue::ScheduleJob(const JobScheduleDesc& desc)
    {
        return ScheduleJobDepends(desc, desc.dependency);
    }

    JobHandle JobQueue::ScheduleJobDepends(const JobScheduleDesc& desc, const JobHandle dependency)
    {
        return ScheduleJobs(
            {desc},
            dependency,
            nullptr,
            nullptr,
            desc.cancelFunction,
            desc.cancelUserData,
            false,
            desc.priority,
            desc.safetyPolicy,
            desc.debugName);
    }

    JobHandle JobQueue::ScheduleJobs(
        std::vector<JobScheduleDesc> jobs,
        const JobHandle dependency,
        const JobFunction completeFunction,
        void* completeUserData,
        const JobFunction cancelFunction,
        void* cancelUserData,
        const bool runCancelOnFailureAfterStart,
        const JobPriority priority,
        const JobSafetyPolicy safetyPolicy,
        const char* debugName,
        const JobFunction terminalCleanupFunction,
        void* terminalCleanupUserData)
    {
        if (jobs.empty() && completeFunction == nullptr)
        {
            Internal::RecordJobViolation(
                JobViolationKind::NullCallback,
                0u,
                "Job scheduling received no callbacks.");
            return {};
        }

        if (std::any_of(
                jobs.begin(),
                jobs.end(),
                [](const JobScheduleDesc& job)
                {
                    return job.function == nullptr;
                }))
        {
            Internal::RecordJobViolation(
                JobViolationKind::NullCallback,
                0u,
                "Job scheduling received a null callback.");
            return {};
        }

        const bool hasDependency = dependency.id != 0u || dependency.generation != 0u;
        const bool dependencyKnown = !hasDependency || Internal::IsKnownJobHandle(dependency);
        if (!dependencyKnown)
        {
            Internal::RecordJobViolation(
                JobViolationKind::InvalidHandle,
                dependency.id,
                "Job scheduling received an unknown dependency handle.");
            return {};
        }

        const bool externalDependency = Internal::IsBackgroundJobHandle(dependency);
        bool retainedExternalDependency = false;
        const auto externalDependencyStatus = externalDependency
            ? Internal::RetainJobCompletionStatus(dependency, retainedExternalDependency)
            : JobCompletionStatus::Unknown;
        if (externalDependency && externalDependencyStatus == JobCompletionStatus::Unknown)
        {
            Internal::RecordJobViolation(
                JobViolationKind::InvalidHandle,
                dependency.id,
                "Job scheduling received an external dependency that could not be retained.");
            return {};
        }

        JobHandle handle;
        std::vector<std::pair<JobFunction, void*>> cleanupActions;
        std::vector<GroupPtr> groupsToRetire;
        bool notifyWorkAvailable = false;
        bool notifyCrossQueueDependency = false;
        {
            std::lock_guard lock(m_mutex);
            if (!m_acceptingWork || m_shutdownRequested)
            {
                if (retainedExternalDependency)
                    Internal::ReleaseJobCompletionStatus(dependency);
                Internal::RecordJobViolation(
                    JobViolationKind::ShutdownSchedulingRejected,
                    0u,
                    "Job scheduling was rejected while the JobSystem is shutting down or not accepting work.");
                return {};
            }

            auto initialDependencyStatus = externalDependency
                ? externalDependencyStatus
                : GetDependencyStatusLocked(dependency);

            auto group = std::make_shared<JobGroup>();
            group->id = m_nextGroupId++;
            group->generation = m_queueGeneration;
            group->priority = priority;
            group->safetyPolicy = safetyPolicy;
            group->debugName = debugName != nullptr ? debugName : "";
            group->dependency = dependency;
            group->jobSafetyPolicies.reserve(jobs.size());
            for (const auto& job : jobs)
                group->jobSafetyPolicies.push_back(job.safetyPolicy);
            group->jobs = std::move(jobs);
            OwnJobDebugNames(group);
            group->completeFunction = completeFunction;
            group->completeUserData = completeUserData;
            group->cancelFunction = cancelFunction;
            group->cancelUserData = cancelUserData;
            group->terminalCleanupFunction = terminalCleanupFunction;
            group->terminalCleanupUserData = terminalCleanupUserData;
            group->runCancelOnFailureAfterStart = runCancelOnFailureAfterStart;

            handle = {group->id, group->generation};
            m_groups.emplace(group->id, group);
            Internal::RecordJobDiagnostic(
                group->id,
                group->generation,
                JobLifecycleState::Created,
                group->debugName.c_str(),
                nullptr,
                dependency.id == 0u ? 0u : 1u);

            if (initialDependencyStatus == JobCompletionStatus::Succeeded)
            {
                notifyWorkAvailable = EnqueueReadyGroupLocked(
                    group,
                    cleanupActions,
                    groupsToRetire,
                    notifyCrossQueueDependency);
            }
            else if (initialDependencyStatus == JobCompletionStatus::Cancelled ||
                initialDependencyStatus == JobCompletionStatus::Failed)
            {
                cleanupActions.push_back(CancelGroupLocked(group, cleanupActions));
                AppendGroupTerminalCleanup(group, cleanupActions);
                QueueGroupForRetirementLocked(group, groupsToRetire);
            }
            else
            {
                group->state = GroupState::WaitingForDependencies;
                Internal::RecordJobDiagnostic(
                    group->id,
                    group->generation,
                    JobLifecycleState::WaitingForDependencies,
                    group->debugName.c_str(),
                    nullptr,
                    dependency.id == 0u ? 0u : 1u);
                if (auto dependencyGroup = FindGroupLocked(dependency))
                    dependencyGroup->dependents.push_back(group->id);
            }
        }

        if (notifyWorkAvailable)
            m_workAvailable.notify_one();
        NotifyCrossQueueDependencyIfNeeded(notifyCrossQueueDependency);
        for (const auto& [cleanupFunction, cleanupUserData] : cleanupActions)
        {
            RunUserCallback(
                cleanupFunction,
                cleanupUserData,
                handle.id,
                "Foreground job cleanup callback threw an exception.");
        }
        RetireQueuedGroupsAfterCallbacks(groupsToRetire, notifyCrossQueueDependency);
        NotifyCrossQueueDependencyIfNeeded(notifyCrossQueueDependency);
        return handle;
    }

    JobHandle JobQueue::ScheduleJobsMultiDepends(
        std::vector<JobScheduleDesc> jobs,
        std::vector<JobHandle> dependencies,
        const JobFunction completeFunction,
        void* completeUserData,
        const JobFunction cancelFunction,
        void* cancelUserData,
        const bool runCancelOnFailureAfterStart,
        const JobPriority priority,
        const JobSafetyPolicy safetyPolicy,
        const char* debugName,
        const JobFunction terminalCleanupFunction,
        void* terminalCleanupUserData)
    {
        if (std::any_of(
                jobs.begin(),
                jobs.end(),
                [](const JobScheduleDesc& job)
                {
                    return job.function == nullptr;
                }))
        {
            Internal::RecordJobViolation(
                JobViolationKind::NullCallback,
                0u,
                "Multi-dependency job scheduling received a null callback.");
            return {};
        }

        for (const JobHandle dependency : dependencies)
        {
            if (!Internal::IsKnownJobHandle(dependency))
            {
                Internal::RecordJobViolation(
                    JobViolationKind::InvalidHandle,
                    dependency.id,
                    "Multi-dependency job scheduling received an unknown dependency handle.");
                return {};
            }
        }

        std::vector<std::pair<JobHandle, JobCompletionStatus>> resolvedExternalDependencies;
        std::vector<JobHandle> retainedExternalDependencies;
        for (const JobHandle dependency : dependencies)
        {
            if (!Internal::IsBackgroundJobHandle(dependency))
                continue;

            bool retained = false;
            const auto status = Internal::RetainJobCompletionStatus(dependency, retained);
            if (retained)
                retainedExternalDependencies.push_back(dependency);
            if (status == JobCompletionStatus::Unknown)
            {
                for (const JobHandle retainedDependency : retainedExternalDependencies)
                    Internal::ReleaseJobCompletionStatus(retainedDependency);
                Internal::RecordJobViolation(
                    JobViolationKind::InvalidHandle,
                    dependency.id,
                    "Multi-dependency job scheduling received an external dependency that could not be retained.");
                return {};
            }
            if (status == JobCompletionStatus::Succeeded ||
                status == JobCompletionStatus::Cancelled ||
                status == JobCompletionStatus::Failed)
                resolvedExternalDependencies.emplace_back(dependency, status);
        }

        JobHandle handle;
        std::vector<std::pair<JobFunction, void*>> cleanupActions;
        std::vector<GroupPtr> groupsToRetire;
        bool notifyWorkAvailable = false;
        bool notifyCrossQueueDependency = false;
        {
            std::lock_guard lock(m_mutex);
            if (!m_acceptingWork || m_shutdownRequested)
            {
                for (const JobHandle dependency : retainedExternalDependencies)
                    Internal::ReleaseJobCompletionStatus(dependency);
                Internal::RecordJobViolation(
                    JobViolationKind::ShutdownSchedulingRejected,
                    0u,
                    "Multi-dependency job scheduling was rejected while the JobSystem is shutting down or not accepting work.");
                return {};
            }

            auto group = std::make_shared<JobGroup>();
            group->id = m_nextGroupId++;
            group->generation = m_queueGeneration;
            group->priority = priority;
            group->safetyPolicy = safetyPolicy;
            group->debugName = debugName != nullptr ? debugName : "";
            group->dependency = dependencies.empty() ? JobHandle{} : dependencies.front();
            group->dependencies = std::move(dependencies);
            group->jobSafetyPolicies.reserve(jobs.size());
            for (const auto& job : jobs)
                group->jobSafetyPolicies.push_back(job.safetyPolicy);
            group->jobs = std::move(jobs);
            OwnJobDebugNames(group);
            group->completeFunction = completeFunction;
            group->completeUserData = completeUserData;
            group->cancelFunction = cancelFunction;
            group->cancelUserData = cancelUserData;
            group->terminalCleanupFunction = terminalCleanupFunction;
            group->terminalCleanupUserData = terminalCleanupUserData;
            group->runCancelOnFailureAfterStart = runCancelOnFailureAfterStart;

            handle = {group->id, group->generation};
            m_groups.emplace(group->id, group);
            Internal::RecordJobDiagnostic(
                group->id,
                group->generation,
                JobLifecycleState::Created,
                group->debugName.c_str(),
                nullptr,
                group->dependencies.size());

            const auto dependencyStatus = GetGroupDependenciesStatusLocked(group, resolvedExternalDependencies);
            if (dependencyStatus == JobCompletionStatus::Succeeded)
            {
                (void)EnqueueReadyGroupLocked(
                    group,
                    cleanupActions,
                    groupsToRetire,
                    notifyCrossQueueDependency);
                notifyWorkAvailable = true;
            }
            else if (dependencyStatus == JobCompletionStatus::Cancelled ||
                dependencyStatus == JobCompletionStatus::Failed)
            {
                cleanupActions.push_back(CancelGroupLocked(group, cleanupActions));
                AppendGroupTerminalCleanup(group, cleanupActions);
                QueueGroupForRetirementLocked(group, groupsToRetire);
            }
            else
            {
                group->state = GroupState::WaitingForDependencies;
                Internal::RecordJobDiagnostic(
                    group->id,
                    group->generation,
                    JobLifecycleState::WaitingForDependencies,
                    group->debugName.c_str(),
                    nullptr,
                    group->dependencies.size());
                RegisterLocalDependentsLocked(group);
            }
        }

        if (notifyWorkAvailable)
            m_workAvailable.notify_one();
        NotifyCrossQueueDependencyIfNeeded(notifyCrossQueueDependency);
        for (const auto& [cleanupFunction, cleanupUserData] : cleanupActions)
        {
            RunUserCallback(
                cleanupFunction,
                cleanupUserData,
                handle.id,
                "Foreground job cleanup callback threw an exception.");
        }
        RetireQueuedGroupsAfterCallbacks(groupsToRetire, notifyCrossQueueDependency);
        NotifyCrossQueueDependencyIfNeeded(notifyCrossQueueDependency);
        return handle;
    }

    JobHandle JobQueue::CreatePendingJob(const JobScheduleDesc& desc)
    {
        return CreatePendingJobs(
            std::vector<JobScheduleDesc>{desc},
            desc.dependency,
            nullptr,
            nullptr,
            desc.cancelFunction,
            desc.cancelUserData,
            false,
            desc.priority,
            desc.safetyPolicy,
            desc.debugName);
    }

    JobHandle JobQueue::CreatePendingJobs(
        std::vector<JobScheduleDesc> jobs,
        const JobHandle dependency,
        const JobFunction completeFunction,
        void* completeUserData,
        const JobFunction cancelFunction,
        void* cancelUserData,
        const bool runCancelOnFailureAfterStart,
        const JobPriority priority,
        const JobSafetyPolicy safetyPolicy,
        const char* debugName,
        const JobFunction terminalCleanupFunction,
        void* terminalCleanupUserData)
    {
        if (std::any_of(
                jobs.begin(),
                jobs.end(),
                [](const JobScheduleDesc& job)
                {
                    return job.function == nullptr;
                }))
        {
            Internal::RecordJobViolation(
                JobViolationKind::NullCallback,
                0u,
                "Pending job scheduling received a null callback.");
            return {};
        }

        if (jobs.empty() && completeFunction == nullptr)
        {
            Internal::RecordJobViolation(
                JobViolationKind::NullCallback,
                0u,
                "Pending job scheduling received no callbacks.");
            return {};
        }

        const bool hasDependency = dependency.id != 0u || dependency.generation != 0u;
        const bool dependencyKnown = !hasDependency || Internal::IsKnownJobHandle(dependency);
        if (!dependencyKnown)
        {
            Internal::RecordJobViolation(
                JobViolationKind::InvalidHandle,
                dependency.id,
                "Pending job scheduling received an unknown dependency handle.");
            return {};
        }

        const bool externalDependency = Internal::IsBackgroundJobHandle(dependency);
        bool retainedExternalDependency = false;
        const auto externalDependencyStatus = externalDependency
            ? Internal::RetainJobCompletionStatus(dependency, retainedExternalDependency)
            : JobCompletionStatus::Unknown;
        if (externalDependency && externalDependencyStatus == JobCompletionStatus::Unknown)
        {
            Internal::RecordJobViolation(
                JobViolationKind::InvalidHandle,
                dependency.id,
                "Pending job scheduling received an external dependency that could not be retained.");
            return {};
        }

        std::lock_guard lock(m_mutex);
        if (!m_acceptingWork || m_shutdownRequested)
        {
            if (retainedExternalDependency)
                Internal::ReleaseJobCompletionStatus(dependency);
            Internal::RecordJobViolation(
                JobViolationKind::ShutdownSchedulingRejected,
                0u,
                "Pending job scheduling was rejected while the JobSystem is shutting down or not accepting work.");
            return {};
        }

        auto group = std::make_shared<JobGroup>();
        group->id = m_nextGroupId++;
        group->generation = m_queueGeneration;
        group->priority = priority;
        group->safetyPolicy = safetyPolicy;
        group->debugName = debugName != nullptr ? debugName : "";
        group->dependency = dependency;
        group->jobSafetyPolicies.reserve(jobs.size());
        for (const auto& job : jobs)
            group->jobSafetyPolicies.push_back(job.safetyPolicy);
        group->jobs = std::move(jobs);
        OwnJobDebugNames(group);
        group->completeFunction = completeFunction;
        group->completeUserData = completeUserData;
        group->cancelFunction = cancelFunction;
        group->cancelUserData = cancelUserData;
        group->terminalCleanupFunction = terminalCleanupFunction;
        group->terminalCleanupUserData = terminalCleanupUserData;
        group->runCancelOnFailureAfterStart = runCancelOnFailureAfterStart;
        group->state = GroupState::PendingKick;

        const JobHandle handle{group->id, group->generation};
        m_groups.emplace(group->id, group);
        Internal::RecordJobDiagnostic(
            group->id,
            group->generation,
            JobLifecycleState::Created,
            group->debugName.c_str(),
            nullptr,
            dependency.id == 0u ? 0u : 1u);
        return handle;
    }

    void JobQueue::KickPending(const JobHandle handle)
    {
        JobHandle dependency;
        {
            std::lock_guard lock(m_mutex);
            auto group = FindGroupLocked(handle);
            if (group == nullptr || group->state != GroupState::PendingKick)
                return;
            dependency = group->dependency;
        }

        const bool externalDependency = Internal::IsBackgroundJobHandle(dependency);
        const auto externalDependencyStatus = externalDependency
            ? Internal::GetJobCompletionStatus(dependency)
            : JobCompletionStatus::Unknown;

        std::vector<std::pair<JobFunction, void*>> cleanupActions;
        std::vector<GroupPtr> groupsToRetire;
        bool notifyWorkAvailable = false;
        bool notifyCrossQueueDependency = false;
        {
            std::lock_guard lock(m_mutex);
            auto group = FindGroupLocked(handle);
            if (group == nullptr || group->state != GroupState::PendingKick)
            {
                return;
            }

            KickPendingGroupLocked(
                group,
                externalDependency,
                externalDependencyStatus,
                cleanupActions,
                groupsToRetire,
                notifyCrossQueueDependency);
            notifyWorkAvailable = true;
        }

        for (const auto& [cleanupFunction, cleanupUserData] : cleanupActions)
        {
            RunUserCallback(
                cleanupFunction,
                cleanupUserData,
                handle.id,
                "Foreground job cleanup callback threw an exception.");
        }
        if (notifyWorkAvailable)
            m_workAvailable.notify_one();
        RetireQueuedGroupsAfterCallbacks(groupsToRetire, notifyCrossQueueDependency);
        NotifyCrossQueueDependencyIfNeeded(notifyCrossQueueDependency);
    }

    void JobQueue::KickAllPending()
    {
        while (true)
        {
            std::vector<JobHandle> pendingHandles;
            {
                std::lock_guard lock(m_mutex);
                for (const auto& [id, group] : m_groups)
                {
                    if (group != nullptr && group->state == GroupState::PendingKick)
                        pendingHandles.push_back({id, group->generation});
                }
            }

            if (pendingHandles.empty())
                break;

            for (const auto handle : pendingHandles)
                KickPending(handle);
        }
    }

    void JobQueue::StopAcceptingWork()
    {
        std::lock_guard lock(m_mutex);
        m_acceptingWork = false;
    }

    void JobQueue::NotifyExternalDependencyChanged()
    {
        {
            std::lock_guard lock(m_mutex);
            NotifyExternalDependencyChangedLocked();
        }
        m_workAvailable.notify_all();
    }

    void JobQueue::NotifyExternalDependencyChangedLocked()
    {
        m_externalDependencyChanged = true;
    }

    void JobQueue::KickPendingGroupLocked(
        const GroupPtr& group,
        const bool externalDependency,
        const JobCompletionStatus externalDependencyStatus,
        std::vector<std::pair<JobFunction, void*>>& cleanupActions,
        std::vector<GroupPtr>& groupsToRetire,
        bool& notifyCrossQueueDependency)
    {
        if (group == nullptr || group->state != GroupState::PendingKick)
            return;

        const auto dependencyStatus = externalDependency
            ? externalDependencyStatus
            : GetDependencyStatusLocked(group->dependency);
        if (dependencyStatus == JobCompletionStatus::Succeeded)
        {
            EnqueueReadyGroupLocked(group, cleanupActions, groupsToRetire, notifyCrossQueueDependency);
        }
        else if (dependencyStatus == JobCompletionStatus::Cancelled ||
            dependencyStatus == JobCompletionStatus::Failed)
        {
            cleanupActions.push_back(CancelGroupLocked(group, cleanupActions));
            AppendGroupTerminalCleanup(group, cleanupActions);
            QueueGroupForRetirementLocked(group, groupsToRetire);
        }
        else
        {
            group->state = GroupState::WaitingForDependencies;
            Internal::RecordJobDiagnostic(
                group->id,
                group->generation,
                JobLifecycleState::WaitingForDependencies,
                group->debugName.c_str(),
                nullptr,
                group->dependency.id == 0u ? 0u : 1u);
            if (auto dependencyGroup = FindGroupLocked(group->dependency))
                dependencyGroup->dependents.push_back(group->id);
        }

        return;
    }

    bool JobQueue::ExecuteOneJob()
    {
        WakeExternalDependencyReadyGroups();

        std::optional<JobExecution> execution;
        {
            std::lock_guard lock(m_mutex);
            execution = PopReadyJobLocked();
        }

        if (!execution.has_value())
            return false;

        for (const auto& [cleanupFunction, cleanupUserData] : execution->cleanupActions)
        {
            RunUserCallback(
                cleanupFunction,
                cleanupUserData,
                execution->group != nullptr ? execution->group->id : 0u,
                "Foreground job cleanup callback threw an exception.");
        }
        ExecuteJob(execution.value());
        return true;
    }

    bool JobQueue::ExecuteOneJobForWait(
        const JobHandle waitedHandle,
        const std::vector<JobHandle>& waitEligibleHandles,
        const bool allowNoSyncWaitOpportunisticWork)
    {
        if (allowNoSyncWaitOpportunisticWork)
            WakeExternalDependencyReadyGroups();

        std::vector<JobHandle> collectedWaitEligibleHandles;
        const std::vector<JobHandle>* effectiveWaitEligibleHandles = &waitEligibleHandles;
        if (waitEligibleHandles.empty())
        {
            collectedWaitEligibleHandles = CollectWaitEligibleForegroundHandles(waitedHandle);
            effectiveWaitEligibleHandles = &collectedWaitEligibleHandles;
        }

        std::optional<JobExecution> execution;
        {
            std::lock_guard lock(m_mutex);
            execution = PopReadyJobForWaitLocked(
                waitedHandle,
                *effectiveWaitEligibleHandles,
                allowNoSyncWaitOpportunisticWork);
        }

        if (!execution.has_value())
            return false;

        for (const auto& [cleanupFunction, cleanupUserData] : execution->cleanupActions)
        {
            RunUserCallback(
                cleanupFunction,
                cleanupUserData,
                execution->group != nullptr ? execution->group->id : 0u,
                "Foreground job cleanup callback threw an exception.");
        }
        ExecuteJob(execution.value());
        return true;
    }

    void JobQueue::Complete(const JobHandle handle)
    {
        while (!IsCompleted(handle))
        {
            if (Internal::DoesJobDependencyChainContainCurrentWorker(handle))
            {
                Internal::RecordJobViolation(
                    JobViolationKind::SyncWaitDisallowed,
                    handle.id,
                    "A job callback cannot synchronously wait for a dependency chain that includes the currently executing job.");
                return;
            }

            const bool currentWorkerIsBackground = Internal::IsExecutingBackgroundJobWorkerThread();
            if (currentWorkerIsBackground)
            {
                for (const JobHandle backgroundDependency : CollectWaitEligibleExternalHandles(handle))
                {
                    if (Internal::IsBackgroundJobHandle(backgroundDependency) &&
                        !Internal::IsBackgroundJobCompleted(backgroundDependency))
                    {
                        Internal::RecordJobViolation(
                            JobViolationKind::SyncWaitDisallowed,
                            handle.id,
                            "A background job callback cannot synchronously wait for a foreground dependency chain that contains a pending background job.");
                        return;
                    }
                }
            }

            for (const JobHandle dependency : CollectGroupDependencies(handle))
            {
                if (Internal::IsCurrentJobWorkerHandle(dependency))
                {
                    Internal::RecordJobViolation(
                        JobViolationKind::SyncWaitDisallowed,
                        handle.id,
                        "A job callback cannot synchronously wait for a job that depends on the currently executing job.");
                    return;
                }
                NLS::Base::Jobs::CompleteNoClear(dependency);
            }

            WakeExternalDependencyReadyGroups();
            const auto waitEligibleHandles = CollectWaitEligibleForegroundHandles(handle);
            {
                std::vector<std::pair<JobFunction, void*>> cleanupActions;
                std::vector<GroupPtr> groupsToRetire;
                bool notifyCrossQueueDependency = false;
                bool resolvedWaitingGroup = false;
                bool shouldKickPending = false;
                {
                    std::lock_guard lock(m_mutex);
                    auto group = FindGroupLocked(handle);
                    if (group != nullptr && group->state == GroupState::PendingKick)
                        shouldKickPending = true;
                    else if (group != nullptr && group->state == GroupState::WaitingForDependencies)
                        resolvedWaitingGroup = TryResolveWaitingGroupLocked(
                            group,
                            cleanupActions,
                            groupsToRetire,
                            notifyCrossQueueDependency);
                }

                for (const auto& [cleanupFunction, cleanupUserData] : cleanupActions)
                {
                    RunUserCallback(
                        cleanupFunction,
                        cleanupUserData,
                        handle.id,
                        "Foreground job cleanup callback threw an exception.");
                }
                if (shouldKickPending)
                    KickPending(handle);
                if (resolvedWaitingGroup)
                    m_workAvailable.notify_all();
                RetireQueuedGroupsAfterCallbacks(groupsToRetire, notifyCrossQueueDependency);
                NotifyCrossQueueDependencyIfNeeded(notifyCrossQueueDependency);
            }
            if (!ExecuteOneJobForWait(handle, waitEligibleHandles))
            {
                std::unique_lock lock(m_mutex);
                m_workAvailable.wait_for(
                    lock,
                    std::chrono::milliseconds(1),
                    [this, handle]
                    {
                        return IsCompletedLocked(handle) ||
                            m_shutdownRequested;
                    });
            }
        }
    }

    std::vector<JobHandle> JobQueue::CollectGroupDependencies(const JobHandle handle) const
    {
        std::lock_guard lock(m_mutex);
        auto group = FindGroupLocked(handle);
        if (group == nullptr || group->state != GroupState::WaitingForDependencies)
            return {};

        if (!group->dependencies.empty())
            return group->dependencies;

        if (group->dependency.id != 0u || group->dependency.generation != 0u)
            return {group->dependency};

        return {};
    }

    std::vector<JobHandle> JobQueue::CollectDirectDependencies(const JobHandle handle) const
    {
        std::lock_guard lock(m_mutex);
        auto group = FindGroupLocked(handle);
        if (group == nullptr ||
            group->state == GroupState::Completed ||
            group->state == GroupState::Cancelled ||
            group->state == GroupState::Failed)
        {
            return {};
        }

        if (!group->dependencies.empty())
            return group->dependencies;

        if (group->dependency.id != 0u || group->dependency.generation != 0u)
            return {group->dependency};

        return {};
    }

    bool JobQueue::IsCompleted(const JobHandle handle) const
    {
        std::lock_guard lock(m_mutex);
        return IsCompletedLocked(handle);
    }

    JobCompletionStatus JobQueue::GetCompletionStatus(const JobHandle handle) const
    {
        std::lock_guard lock(m_mutex);
        if (handle.id == 0u && handle.generation == 0u)
            return JobCompletionStatus::Succeeded;

        if (IsRetiredHandleLocked(handle))
            return GetRetiredStatusLocked(handle);

        const auto group = FindGroupLocked(handle);
        if (group == nullptr)
            return JobCompletionStatus::Unknown;

        return GetGroupCompletionStatusLocked(group);
    }

    bool JobQueue::IsKnownHandle(const JobHandle handle) const
    {
        std::lock_guard lock(m_mutex);
        return FindGroupLocked(handle) != nullptr || IsRetiredHandleLocked(handle);
    }

    void JobQueue::ClearWithoutSync(const JobHandle handle)
    {
        (void)handle;
    }

    uint32_t JobQueue::GetWorkerCount() const
    {
        std::lock_guard lock(m_mutex);
        return static_cast<uint32_t>(m_workers.size());
    }

    JobQueue::GroupPtr JobQueue::FindGroupLocked(const JobHandle handle) const
    {
        if (handle.id == 0u && handle.generation == 0u)
            return nullptr;

        const auto found = m_groups.find(handle.id);
        if (found == m_groups.end())
            return nullptr;

        const auto& group = found->second;
        if (group == nullptr || group->generation != handle.generation)
            return nullptr;

        return group;
    }

    void JobQueue::AppendGroupTerminalCleanup(
        const GroupPtr& group,
        std::vector<std::pair<JobFunction, void*>>& cleanupActions)
    {
        if (group == nullptr || group->terminalCleanupConsumed)
            return;

        group->terminalCleanupConsumed = true;
        const auto cleanupFunction = group->terminalCleanupFunction;
        const auto cleanupUserData = group->terminalCleanupUserData;
        group->terminalCleanupFunction = nullptr;
        group->terminalCleanupUserData = nullptr;
        if (cleanupFunction != nullptr)
            cleanupActions.emplace_back(cleanupFunction, cleanupUserData);
    }

    void JobQueue::OwnJobDebugNames(const GroupPtr& group)
    {
        if (group == nullptr)
            return;

        group->jobDebugNames.clear();
        group->jobDebugNames.reserve(group->jobs.size());
        for (const auto& job : group->jobs)
            group->jobDebugNames.emplace_back(job.debugName != nullptr ? job.debugName : "");

        for (size_t index = 0u; index < group->jobs.size(); ++index)
        {
            group->jobs[index].debugName = group->jobDebugNames[index].empty()
                ? nullptr
                : group->jobDebugNames[index].c_str();
        }
    }

    void JobQueue::AppendUnstartedJobCancelCallbacks(
        const GroupPtr& group,
        std::vector<std::pair<JobFunction, void*>>& cleanupActions)
    {
        if (group == nullptr)
            return;

        for (size_t jobIndex = group->nextJobIndex; jobIndex < group->jobs.size(); ++jobIndex)
        {
            auto& job = group->jobs[jobIndex];
            const auto cancelFunction = job.cancelFunction;
            const auto cancelUserData = job.cancelUserData;
            job.cancelFunction = nullptr;
            job.cancelUserData = nullptr;
            if (cancelFunction != nullptr)
                cleanupActions.emplace_back(cancelFunction, cancelUserData);
        }
    }

    bool JobQueue::IsCompletedLocked(const JobHandle handle) const
    {
        if (handle.id == 0u && handle.generation == 0u)
            return true;

        if (IsRetiredHandleLocked(handle))
            return true;

        const auto group = FindGroupLocked(handle);
        if (group == nullptr)
            return true;

        return IsGroupFinishedLocked(group);
    }

    JobCompletionStatus JobQueue::GetDependencyStatusLocked(const JobHandle dependency) const
    {
        if (dependency.id == 0u && dependency.generation == 0u)
            return JobCompletionStatus::Succeeded;

        if (IsRetiredHandleLocked(dependency))
            return GetRetiredStatusLocked(dependency);

        const auto group = FindGroupLocked(dependency);
        if (group == nullptr)
            return JobCompletionStatus::Unknown;

        return GetGroupCompletionStatusLocked(group);
    }

    bool JobQueue::IsExternalDependencyLocked(const JobHandle dependency) const
    {
        return dependency.id != 0u &&
            FindGroupLocked(dependency) == nullptr &&
            !IsRetiredHandleLocked(dependency);
    }

    bool JobQueue::IsGroupFinishedLocked(const GroupPtr& group) const
    {
        return group != nullptr &&
            ((group->state == GroupState::Completed && !group->completeCallbackRunning) ||
                ((group->state == GroupState::Cancelled || group->state == GroupState::Failed) &&
                    group->runningJobCount == 0u &&
                    !group->completeCallbackRunning &&
                    group->cleanupConsumed &&
                    group->terminalCleanupConsumed));
    }

    bool JobQueue::HasUnfinishedGroupsLocked() const
    {
        return std::any_of(
            m_groups.begin(),
            m_groups.end(),
            [this](const auto& entry)
            {
                return entry.second != nullptr && !IsGroupFinishedLocked(entry.second);
            });
    }

    JobCompletionStatus JobQueue::GetGroupCompletionStatusLocked(const GroupPtr& group) const
    {
        if (group == nullptr)
            return JobCompletionStatus::Unknown;

        switch (group->state)
        {
        case GroupState::Completed:
            return !group->completeCallbackRunning
                ? JobCompletionStatus::Succeeded
                : JobCompletionStatus::Pending;
        case GroupState::Cancelled:
            return IsGroupFinishedLocked(group)
                ? JobCompletionStatus::Cancelled
                : JobCompletionStatus::Pending;
        case GroupState::Failed:
            return IsGroupFinishedLocked(group)
                ? JobCompletionStatus::Failed
                : JobCompletionStatus::Pending;
        default:
            return JobCompletionStatus::Pending;
        }
    }

    JobCompletionStatus JobQueue::FindResolvedDependencyStatus(
        const JobHandle dependency,
        const std::vector<std::pair<JobHandle, JobCompletionStatus>>& resolvedExternalDependencies)
    {
        const auto found = std::find_if(
            resolvedExternalDependencies.begin(),
            resolvedExternalDependencies.end(),
            [dependency](const std::pair<JobHandle, JobCompletionStatus>& resolved)
            {
                return resolved.first.id == dependency.id &&
                    resolved.first.generation == dependency.generation;
            });
        return found != resolvedExternalDependencies.end()
            ? found->second
            : JobCompletionStatus::Unknown;
    }

    JobCompletionStatus JobQueue::GetGroupDependenciesStatusLocked(const GroupPtr& group) const
    {
        return GetGroupDependenciesStatusLocked(group, {});
    }

    JobCompletionStatus JobQueue::GetGroupDependenciesStatusLocked(
        const GroupPtr& group,
        const std::vector<std::pair<JobHandle, JobCompletionStatus>>& resolvedExternalDependencies) const
    {
        if (group == nullptr)
            return JobCompletionStatus::Unknown;

        bool hasPendingDependency = false;
        auto checkDependency = [this, &hasPendingDependency, &resolvedExternalDependencies](const JobHandle dependency) -> JobCompletionStatus
        {
            auto dependencyStatus = FindResolvedDependencyStatus(dependency, resolvedExternalDependencies);
            if (dependencyStatus == JobCompletionStatus::Unknown)
                dependencyStatus = GetDependencyStatusLocked(dependency);
            if (dependencyStatus == JobCompletionStatus::Unknown &&
                IsExternalDependencyLocked(dependency))
            {
                dependencyStatus = FindResolvedDependencyStatus(dependency, resolvedExternalDependencies);
            }
            if (dependencyStatus == JobCompletionStatus::Cancelled ||
                dependencyStatus == JobCompletionStatus::Failed)
                return dependencyStatus;
            if (dependencyStatus != JobCompletionStatus::Succeeded)
                hasPendingDependency = true;
            return JobCompletionStatus::Pending;
        };

        if (group->dependencies.empty())
        {
            const auto status = checkDependency(group->dependency);
            if (status == JobCompletionStatus::Cancelled ||
                status == JobCompletionStatus::Failed)
                return status;
        }
        else
        {
            for (const JobHandle dependency : group->dependencies)
            {
                const auto status = checkDependency(dependency);
                if (status == JobCompletionStatus::Cancelled ||
                    status == JobCompletionStatus::Failed)
                    return status;
            }
        }

        return hasPendingDependency
            ? JobCompletionStatus::Pending
            : JobCompletionStatus::Succeeded;
    }

    void JobQueue::RegisterLocalDependentsLocked(const GroupPtr& group)
    {
        if (group == nullptr)
            return;

        auto registerDependency = [this, &group](const JobHandle dependency)
        {
            if (Internal::IsBackgroundJobHandle(dependency))
                return;

            if (auto dependencyGroup = FindGroupLocked(dependency))
            {
                const bool alreadyRegistered = std::any_of(
                    dependencyGroup->dependents.begin(),
                    dependencyGroup->dependents.end(),
                    [&group](const uint64_t dependentId)
                    {
                        return dependentId == group->id;
                    });
                if (!alreadyRegistered)
                    dependencyGroup->dependents.push_back(group->id);
            }
        };

        if (group->dependencies.empty())
            registerDependency(group->dependency);
        else
            for (const JobHandle dependency : group->dependencies)
                registerDependency(dependency);
    }

    bool JobQueue::IsRetiredHandleLocked(const JobHandle handle) const
    {
        const auto found = m_retiredGenerations.find(handle.id);
        return found != m_retiredGenerations.end() && found->second == handle.generation;
    }

    JobCompletionStatus JobQueue::GetRetiredStatusLocked(const JobHandle handle) const
    {
        const auto found = m_retiredStatuses.find(handle.id);
        if (found == m_retiredStatuses.end())
            return JobCompletionStatus::Unknown;

        return found->second;
    }

    bool JobQueue::RetireGroupLocked(const GroupPtr& group)
    {
        if (group == nullptr)
            return false;

        ReleaseExternalDependenciesForGroupLocked(group);
        const JobCompletionStatus status = GetGroupCompletionStatusLocked(group);
        m_retiredGenerations[group->id] = group->generation;
        m_retiredStatuses[group->id] = status;
        const bool notifyCrossQueueDependency =
            Internal::RecordJobTerminalStatus({group->id, group->generation}, status);
        NotifyExternalDependencyChangedLocked();
        m_retiredOrder.push_back(group->id);
        m_groups.erase(group->id);
        PruneRetiredHandlesLocked();
        return notifyCrossQueueDependency;
    }

    void JobQueue::QueueGroupForRetirementLocked(
        const GroupPtr& group,
        std::vector<GroupPtr>& groupsToRetire)
    {
        if (group == nullptr)
            return;

        const auto alreadyQueued = std::any_of(
            groupsToRetire.begin(),
            groupsToRetire.end(),
            [&group](const GroupPtr& queued)
            {
                return queued != nullptr &&
                    queued->id == group->id &&
                    queued->generation == group->generation;
            });
        if (alreadyQueued)
            return;

        group->completeCallbackRunning = true;
        groupsToRetire.push_back(group);
    }

    void JobQueue::RetireQueuedGroupsAfterCallbacks(
        const std::vector<GroupPtr>& groupsToRetire,
        bool& notifyCrossQueueDependency)
    {
        if (groupsToRetire.empty())
            return;

        bool retiredAnyGroup = false;
        {
            std::lock_guard lock(m_mutex);
            for (const auto& group : groupsToRetire)
            {
                if (group == nullptr)
                    continue;
                if (FindGroupLocked({group->id, group->generation}) == nullptr)
                    continue;
                group->completeCallbackRunning = false;
                notifyCrossQueueDependency |= RetireGroupLocked(group);
                retiredAnyGroup = true;
            }
        }

        if (retiredAnyGroup)
        {
            m_workAvailable.notify_all();
        }
    }

    void JobQueue::ReleaseExternalDependenciesForGroupLocked(const GroupPtr& group) const
    {
        if (group == nullptr)
            return;

        auto releaseDependency = [](const JobHandle dependency)
        {
            if (Internal::IsBackgroundJobHandle(dependency))
                Internal::ReleaseJobCompletionStatus(dependency);
        };

        if (group->dependencies.empty())
        {
            releaseDependency(group->dependency);
            return;
        }

        for (const JobHandle dependency : group->dependencies)
            releaseDependency(dependency);
    }

    void JobQueue::PruneRetiredHandlesLocked()
    {
        while (m_retiredOrder.size() > kRetiredHandleHistoryLimit)
        {
            const uint64_t retiredId = m_retiredOrder.front();
            m_retiredOrder.pop_front();
            m_retiredGenerations.erase(retiredId);
            m_retiredStatuses.erase(retiredId);
        }
    }

    std::pair<JobFunction, void*> JobQueue::CancelGroupLocked(
        const GroupPtr& group,
        std::vector<std::pair<JobFunction, void*>>& cleanupActions)
    {
        if (group == nullptr)
            return {};

        group->state = GroupState::Cancelled;
        Internal::RecordJobDiagnostic(
            group->id,
            group->generation,
            JobLifecycleState::Cancelled,
            group->debugName.c_str(),
            nullptr,
            group->dependency.id == 0u ? 0u : 1u);

        if (group->cleanupConsumed)
            return {};

        group->cleanupConsumed = true;
        const bool hasGroupCancelFunction = group->cancelFunction != nullptr;
        const auto cancelFunction = group->cancelFunction;
        const auto cancelUserData = group->cancelUserData;
        group->cancelFunction = nullptr;
        group->cancelUserData = nullptr;
        if (!hasGroupCancelFunction)
            AppendUnstartedJobCancelCallbacks(group, cleanupActions);
        return {cancelFunction, cancelUserData};
    }

    bool JobQueue::EnqueueReadyGroupLocked(
        const GroupPtr& group,
        std::vector<std::pair<JobFunction, void*>>& cleanupActions,
        std::vector<GroupPtr>& groupsToRetire,
        bool& notifyCrossQueueDependency)
    {
        if (group == nullptr ||
            group->state == GroupState::Completed ||
            group->state == GroupState::Cancelled ||
            group->state == GroupState::Failed)
        {
            return false;
        }

        if (group->jobs.empty() && group->completeFunction == nullptr)
        {
            CompleteDependencyOnlyGroupLocked(group, cleanupActions, groupsToRetire, notifyCrossQueueDependency);
            return true;
        }

        group->state = GroupState::Queued;
        if (!group->queuedDiagnosticRecorded)
        {
            group->queuedDiagnosticRecorded = true;
            Internal::RecordJobDiagnostic(
                group->id,
                group->generation,
                JobLifecycleState::Queued,
                group->debugName.c_str(),
                nullptr,
                group->dependencies.empty() ? (group->dependency.id == 0u ? 0u : 1u) : group->dependencies.size());
        }
        if (group->priority == JobPriority::High)
            m_highPriorityQueue.push_back(group->id);
        else
            m_normalPriorityQueue.push_back(group->id);
        return true;
    }

    void JobQueue::CompleteDependencyOnlyGroupLocked(
        const GroupPtr& group,
        std::vector<std::pair<JobFunction, void*>>& cleanupActions,
        std::vector<GroupPtr>& groupsToRetire,
        bool& notifyCrossQueueDependency)
    {
        if (group == nullptr)
            return;

        std::vector<GroupPtr> readyDependents;
            group->state = GroupState::Completed;
            Internal::RecordJobDiagnostic(
                group->id,
                group->generation,
                JobLifecycleState::Completed,
                group->debugName.c_str(),
                nullptr,
                group->dependencies.empty() ? (group->dependency.id == 0u ? 0u : 1u) : group->dependencies.size());
        WakeDependentsForFinishedGroupLocked(
            group,
            readyDependents,
            cleanupActions,
            groupsToRetire,
            notifyCrossQueueDependency);
        AppendGroupTerminalCleanup(group, cleanupActions);
        QueueGroupForRetirementLocked(group, groupsToRetire);
    }

    void JobQueue::WakeExternalDependencyReadyGroups()
    {
        std::vector<JobHandle> dependenciesToCheck;
        {
            std::lock_guard lock(m_mutex);
            for (const auto& [id, group] : m_groups)
            {
                (void)id;
                if (group == nullptr ||
                    group->state != GroupState::WaitingForDependencies)
                {
                    continue;
                }

                if (group->dependencies.empty() &&
                    group->dependency.id != 0u &&
                    group->dependency.generation != 0u &&
                    group->dependency.id == group->id &&
                    group->dependency.generation == group->generation)
                {
                    continue;
                }

                CollectExternalDependenciesForGroupLocked(group, dependenciesToCheck);
            }
        }

        if (dependenciesToCheck.empty())
            return;

        std::vector<std::pair<JobHandle, JobCompletionStatus>> resolvedDependencies;
        resolvedDependencies.reserve(dependenciesToCheck.size());
        for (const JobHandle dependency : dependenciesToCheck)
        {
            const auto status = Internal::GetJobCompletionStatus(dependency);
            if (status == JobCompletionStatus::Succeeded ||
                status == JobCompletionStatus::Cancelled ||
                status == JobCompletionStatus::Failed)
                resolvedDependencies.emplace_back(dependency, status);
        }

        if (resolvedDependencies.empty())
            return;

        bool wokeGroup = false;
        bool notifyCrossQueueDependency = false;
        std::vector<std::pair<JobFunction, void*>> cleanupActions;
        std::vector<GroupPtr> groupsToRetire;
        {
            std::lock_guard lock(m_mutex);
            std::vector<uint64_t> groupsToResolve;
            for (const auto& [id, group] : m_groups)
            {
                if (group == nullptr || group->state != GroupState::WaitingForDependencies)
                    continue;

                std::vector<JobHandle> externalDependencies;
                CollectExternalDependenciesForGroupLocked(group, externalDependencies);
                if (externalDependencies.empty())
                    continue;

                groupsToResolve.push_back(id);
            }

            for (const uint64_t groupId : groupsToResolve)
            {
                const auto found = m_groups.find(groupId);
                if (found == m_groups.end() || found->second == nullptr)
                    continue;

                auto group = found->second;
                if (group->state != GroupState::WaitingForDependencies)
                {
                    continue;
                }

                const auto dependencyStatus = GetGroupDependenciesStatusLocked(group, resolvedDependencies);
                if (dependencyStatus == JobCompletionStatus::Pending)
                    continue;

                if (dependencyStatus == JobCompletionStatus::Succeeded)
                {
                    EnqueueReadyGroupLocked(
                        group,
                        cleanupActions,
                        groupsToRetire,
                        notifyCrossQueueDependency);
                    wokeGroup = true;
                }
                else
            {
                group->state = GroupState::Cancelled;
                AppendGroupTerminalCleanup(group, cleanupActions);
                Internal::RecordJobDiagnostic(
                    group->id,
                        group->generation,
                        JobLifecycleState::Cancelled,
                        group->debugName.c_str(),
                        nullptr,
                        group->dependency.id == 0u ? 0u : 1u);
                    if (!group->cleanupConsumed)
                    {
                        group->cleanupConsumed = true;
                        const bool hasGroupCancelFunction = group->cancelFunction != nullptr;
                        const auto cancelFunction = group->cancelFunction;
                        const auto cancelUserData = group->cancelUserData;
                        group->cancelFunction = nullptr;
                        group->cancelUserData = nullptr;
                        if (cancelFunction != nullptr)
                            cleanupActions.emplace_back(cancelFunction, cancelUserData);
                        if (!hasGroupCancelFunction)
                            AppendUnstartedJobCancelCallbacks(group, cleanupActions);
                    }
                    std::vector<GroupPtr> readyDependents;
                    WakeDependentsForFinishedGroupLocked(
                        group,
                        readyDependents,
                        cleanupActions,
                        groupsToRetire,
                        notifyCrossQueueDependency);
                    AppendGroupTerminalCleanup(group, cleanupActions);
                    QueueGroupForRetirementLocked(group, groupsToRetire);
                    wokeGroup = true;
                }
            }
        }

        for (const auto& [cleanupFunction, cleanupUserData] : cleanupActions)
        {
            RunUserCallback(
                cleanupFunction,
                cleanupUserData,
                0u,
                "Foreground job cleanup callback threw an exception.");
        }
        RetireQueuedGroupsAfterCallbacks(groupsToRetire, notifyCrossQueueDependency);

        if (wokeGroup)
            m_workAvailable.notify_all();
        NotifyCrossQueueDependencyIfNeeded(notifyCrossQueueDependency);
    }

    void JobQueue::CollectExternalDependenciesForGroupLocked(
        const GroupPtr& group,
        std::vector<JobHandle>& dependencies) const
    {
        if (group == nullptr)
            return;

        auto addExternalDependency = [this, &dependencies](const JobHandle dependency)
        {
            if (dependency.id == 0u || !IsExternalDependencyLocked(dependency))
                return;

            const bool alreadyQueued = std::any_of(
                dependencies.begin(),
                dependencies.end(),
                [dependency](const JobHandle existing)
                {
                    return existing.id == dependency.id &&
                        existing.generation == dependency.generation;
                });
            if (!alreadyQueued)
                dependencies.push_back(dependency);
        };

        if (group->dependencies.empty())
        {
            addExternalDependency(group->dependency);
            return;
        }

        for (const JobHandle dependency : group->dependencies)
            addExternalDependency(dependency);
    }

    bool JobQueue::TryResolveWaitingGroupLocked(
        const GroupPtr& group,
        std::vector<std::pair<JobFunction, void*>>& cleanupActions,
        std::vector<GroupPtr>& groupsToRetire,
        bool& notifyCrossQueueDependency)
    {
        return TryResolveWaitingGroupLocked(group, {}, cleanupActions, groupsToRetire, notifyCrossQueueDependency);
    }

    bool JobQueue::TryResolveWaitingGroupLocked(
        const GroupPtr& group,
        const std::vector<std::pair<JobHandle, JobCompletionStatus>>& resolvedExternalDependencies,
        std::vector<std::pair<JobFunction, void*>>& cleanupActions,
        std::vector<GroupPtr>& groupsToRetire,
        bool& notifyCrossQueueDependency)
    {
        if (group == nullptr || group->state != GroupState::WaitingForDependencies)
            return false;

        const auto dependencyStatus = GetGroupDependenciesStatusLocked(group, resolvedExternalDependencies);
        if (dependencyStatus == JobCompletionStatus::Pending)
            return false;

        if (dependencyStatus == JobCompletionStatus::Succeeded)
        {
            return EnqueueReadyGroupLocked(group, cleanupActions, groupsToRetire, notifyCrossQueueDependency);
        }

        group->state = GroupState::Cancelled;
        Internal::RecordJobDiagnostic(
            group->id,
            group->generation,
            JobLifecycleState::Cancelled,
            group->debugName.c_str(),
            nullptr,
            group->dependencies.empty() ? (group->dependency.id == 0u ? 0u : 1u) : group->dependencies.size());
        if (!group->cleanupConsumed)
        {
            group->cleanupConsumed = true;
            const bool hasGroupCancelFunction = group->cancelFunction != nullptr;
            const auto cancelFunction = group->cancelFunction;
            const auto cancelUserData = group->cancelUserData;
            group->cancelFunction = nullptr;
            group->cancelUserData = nullptr;
            if (cancelFunction != nullptr)
                cleanupActions.emplace_back(cancelFunction, cancelUserData);
            if (!hasGroupCancelFunction)
                AppendUnstartedJobCancelCallbacks(group, cleanupActions);
        }
        std::vector<GroupPtr> readyDependents;
        WakeDependentsForFinishedGroupLocked(
            group,
            readyDependents,
            cleanupActions,
            groupsToRetire,
            notifyCrossQueueDependency);
        AppendGroupTerminalCleanup(group, cleanupActions);
        QueueGroupForRetirementLocked(group, groupsToRetire);
        return true;
    }

    bool JobQueue::HasExternalDependencyWaiterLocked() const
    {
        return std::any_of(
            m_groups.begin(),
            m_groups.end(),
            [this](const auto& entry)
            {
                const auto& group = entry.second;
                return group != nullptr &&
                    group->state == GroupState::WaitingForDependencies &&
                    [&]
                    {
                        std::vector<JobHandle> externalDependencies;
                        CollectExternalDependenciesForGroupLocked(group, externalDependencies);
                        return !externalDependencies.empty();
                    }();
            });
    }

    JobQueue::GroupPtr JobQueue::PopReadyGroupLocked()
    {
        auto popFromQueue = [this](std::deque<uint64_t>& queue, const bool highPriority) -> GroupPtr
        {
            while (!queue.empty())
            {
                const uint64_t groupId = queue.front();
                queue.pop_front();
                const auto found = m_groups.find(groupId);
                if (found == m_groups.end() || found->second == nullptr)
                    continue;

                auto group = found->second;
                if (group->state == GroupState::Queued)
                {
                    group->state = GroupState::Running;
                    m_highPriorityDispatchStreak = highPriority
                        ? m_highPriorityDispatchStreak + 1u
                        : 0u;
                    return group;
                }
            }

            return nullptr;
        };

        if (!m_highPriorityQueue.empty() &&
            (m_normalPriorityQueue.empty() || m_highPriorityDispatchStreak < kMaxHighPriorityDispatchStreak))
        {
            if (auto group = popFromQueue(m_highPriorityQueue, true))
                return group;
        }

        if (auto group = popFromQueue(m_normalPriorityQueue, false))
            return group;

        return popFromQueue(m_highPriorityQueue, true);
    }

    std::optional<JobQueue::JobExecution> JobQueue::PopReadyJobLocked()
    {
        auto group = PopReadyGroupLocked();
        if (group == nullptr)
            return std::nullopt;

        if (group->state == GroupState::Cancelled || group->state == GroupState::Failed)
            return std::nullopt;

        if (group->nextJobIndex >= group->jobs.size() && group->completeFunction == nullptr)
            return std::nullopt;

        JobExecution execution;
        execution.group = group;
        if (group->nextJobIndex < group->jobs.size())
        {
            execution.jobIndex = group->nextJobIndex;
            execution.job = group->jobs[group->nextJobIndex++];
            if (group->nextJobIndex < group->jobs.size())
            {
                std::vector<GroupPtr> unusedGroupsToRetire;
                bool unusedNotifyCrossQueueDependency = false;
                EnqueueReadyGroupLocked(
                    group,
                    execution.cleanupActions,
                    unusedGroupsToRetire,
                    unusedNotifyCrossQueueDependency);
            }
        }
        else
        {
            execution.jobIndex = 0u;
            execution.job.function = group->completeFunction;
            execution.job.userData = group->completeUserData;
            execution.job.debugName = group->debugName.c_str();
            group->completeFunction = nullptr;
            group->completeUserData = nullptr;
            group->cleanupConsumed = true;
        }
        execution.recordRunningDiagnostic = !group->runningDiagnosticRecorded;
        group->runningDiagnosticRecorded = true;
        ++group->runningJobCount;

        return execution;
    }

    std::optional<JobQueue::JobExecution> JobQueue::PopReadyJobForWaitLocked(
        const JobHandle waitedHandle,
        const std::vector<JobHandle>& waitEligibleHandles,
        const bool allowNoSyncWaitOpportunisticWork)
    {
        auto isWaitEligible = [waitedHandle, &waitEligibleHandles](const GroupPtr& group)
        {
            if (group == nullptr)
                return false;

            if (group->id == waitedHandle.id && group->generation == waitedHandle.generation)
                return true;

            return std::any_of(
                waitEligibleHandles.begin(),
                waitEligibleHandles.end(),
                [&group](const JobHandle handle)
                {
                    return group->id == handle.id && group->generation == handle.generation;
                });
        };

        auto extractCandidate = [this, &isWaitEligible, allowNoSyncWaitOpportunisticWork](std::deque<uint64_t>& queue) -> GroupPtr
        {
            for (auto iter = queue.begin(); iter != queue.end(); ++iter)
            {
                const uint64_t groupId = *iter;
                const auto found = m_groups.find(groupId);
                if (found == m_groups.end() || found->second == nullptr)
                    continue;

                auto group = found->second;
                if (group->state != GroupState::Queued)
                    continue;

                const bool canRunWhileWaiting =
                    (allowNoSyncWaitOpportunisticWork &&
                        group->safetyPolicy == JobSafetyPolicy::GuaranteedNoSyncWait) ||
                    isWaitEligible(group);
                if (!canRunWhileWaiting)
                    continue;

                queue.erase(iter);
                group->state = GroupState::Running;
                return group;
            }

            return nullptr;
        };

        auto group = extractCandidate(m_highPriorityQueue);
        if (group == nullptr)
            group = extractCandidate(m_normalPriorityQueue);
        if (group == nullptr ||
            (group->nextJobIndex >= group->jobs.size() && group->completeFunction == nullptr))
        {
            return std::nullopt;
        }

        if (group->state == GroupState::Cancelled || group->state == GroupState::Failed)
            return std::nullopt;

        JobExecution execution;
        execution.group = group;
        if (group->nextJobIndex < group->jobs.size())
        {
            execution.jobIndex = group->nextJobIndex;
            execution.job = group->jobs[group->nextJobIndex++];
            if (group->nextJobIndex < group->jobs.size())
            {
                std::vector<GroupPtr> unusedGroupsToRetire;
                bool unusedNotifyCrossQueueDependency = false;
                EnqueueReadyGroupLocked(
                    group,
                    execution.cleanupActions,
                    unusedGroupsToRetire,
                    unusedNotifyCrossQueueDependency);
            }
        }
        else
        {
            execution.jobIndex = 0u;
            execution.job.function = group->completeFunction;
            execution.job.userData = group->completeUserData;
            execution.job.debugName = group->debugName.c_str();
            group->completeFunction = nullptr;
            group->completeUserData = nullptr;
            group->cleanupConsumed = true;
        }
        execution.recordRunningDiagnostic = !group->runningDiagnosticRecorded;
        group->runningDiagnosticRecorded = true;
        ++group->runningJobCount;

        return execution;
    }

    void JobQueue::CollectWaitEligibleHandlesLocked(
        const JobHandle handle,
        std::vector<JobHandle>& foregroundHandles,
        std::vector<JobHandle>& externalHandles) const
    {
        if (handle.id == 0u && handle.generation == 0u)
            return;

        if (IsExternalDependencyLocked(handle))
        {
            if (std::none_of(
                    externalHandles.begin(),
                    externalHandles.end(),
                    [handle](const JobHandle existing)
                    {
                        return existing.id == handle.id && existing.generation == handle.generation;
                    }))
            {
                externalHandles.push_back(handle);
            }
            return;
        }

        auto group = FindGroupLocked(handle);
        std::vector<GroupPtr> pendingGroups;
        pendingGroups.push_back(group);
        while (!pendingGroups.empty())
        {
            group = pendingGroups.back();
            pendingGroups.pop_back();
            if (group == nullptr)
                continue;

            const JobHandle groupHandle{group->id, group->generation};
            if (std::any_of(
                    foregroundHandles.begin(),
                    foregroundHandles.end(),
                    [groupHandle](const JobHandle existing)
                    {
                        return existing.id == groupHandle.id && existing.generation == groupHandle.generation;
                    }))
            {
                continue;
            }

            foregroundHandles.push_back(groupHandle);
            const auto visitDependency = [this, &externalHandles, &pendingGroups](
                const JobHandle dependency)
            {
                if (dependency.id == 0u && dependency.generation == 0u)
                    return;

                if (IsExternalDependencyLocked(dependency))
                {
                    if (std::none_of(
                        externalHandles.begin(),
                        externalHandles.end(),
                        [dependency](const JobHandle existing)
                        {
                            return existing.id == dependency.id && existing.generation == dependency.generation;
                        }))
                    {
                        externalHandles.push_back(dependency);
                    }
                    return;
                }

                if (auto dependencyGroup = FindGroupLocked(dependency))
                    pendingGroups.push_back(dependencyGroup);
            };

            if (group->dependencies.empty())
            {
                visitDependency(group->dependency);
                continue;
            }

            for (const JobHandle dependency : group->dependencies)
                visitDependency(dependency);
        }
    }

    void JobQueue::CollectWaitEligibleHandles(
        const JobHandle waitedHandle,
        std::vector<JobHandle>& foregroundHandles,
        std::vector<JobHandle>& externalHandles) const
    {
        {
            std::lock_guard lock(m_mutex);
            CollectWaitEligibleHandlesLocked(waitedHandle, foregroundHandles, externalHandles);
        }

        for (size_t externalIndex = 0u; externalIndex < externalHandles.size(); ++externalIndex)
        {
            const JobHandle externalHandle = externalHandles[externalIndex];
            struct VisitContext
            {
                const JobQueue* queue = nullptr;
                std::vector<JobHandle>* foregroundHandles = nullptr;
                std::vector<JobHandle>* externalHandles = nullptr;
            };
            VisitContext context{this, &foregroundHandles, &externalHandles};
            Internal::VisitForegroundDependenciesForBackgroundJob(
                externalHandle,
                [](const JobHandle dependencyHandle, void* userData)
                {
                    auto* context = static_cast<VisitContext*>(userData);
                    std::vector<JobHandle> nestedExternalHandles;
                    {
                        std::lock_guard lock(context->queue->m_mutex);
                        context->queue->CollectWaitEligibleHandlesLocked(
                            dependencyHandle,
                            *context->foregroundHandles,
                            nestedExternalHandles);
                    }
                    for (const JobHandle nestedExternalHandle : nestedExternalHandles)
                    {
                        if (std::none_of(
                                context->externalHandles->begin(),
                                context->externalHandles->end(),
                                [nestedExternalHandle](const JobHandle existing)
                                {
                                    return existing.id == nestedExternalHandle.id &&
                                        existing.generation == nestedExternalHandle.generation;
                                }))
                        {
                            context->externalHandles->push_back(nestedExternalHandle);
                        }
                    }
                    return true;
                },
                &context);
        }
    }

    std::vector<JobHandle> JobQueue::CollectWaitEligibleForegroundHandles(const JobHandle waitedHandle) const
    {
        std::vector<JobHandle> foregroundHandles;
        std::vector<JobHandle> externalHandles;
        CollectWaitEligibleHandles(waitedHandle, foregroundHandles, externalHandles);

        return foregroundHandles;
    }

    std::vector<JobHandle> JobQueue::CollectWaitEligibleExternalHandles(const JobHandle waitedHandle) const
    {
        std::vector<JobHandle> foregroundHandles;
        std::vector<JobHandle> externalHandles;
        CollectWaitEligibleHandles(waitedHandle, foregroundHandles, externalHandles);

        return externalHandles;
    }

    void JobQueue::ExecuteJob(const JobExecution& execution)
    {
        if (execution.group == nullptr)
            return;

        bool failed = false;
        const JobHandle groupHandle{execution.group->id, execution.group->generation};
        if (Internal::AreJobDiagnosticsEnabled() && execution.recordRunningDiagnostic)
        {
            std::ostringstream workerNameStream;
            workerNameStream << "Job Thread " << std::this_thread::get_id();
            const std::string workerName = workerNameStream.str();
            Internal::RecordJobDiagnostic(
                execution.group->id,
                execution.group->generation,
                JobLifecycleState::Running,
                execution.group->debugName.c_str(),
                workerName.c_str(),
                execution.group->dependency.id == 0u ? 0u : 1u);
        }
        {
            const std::string& groupDebugName = execution.group->debugName;
            const char* jobDebugName = execution.job.debugName;
            const char* scopeName = jobDebugName != nullptr
                ? jobDebugName
                : (!groupDebugName.empty() ? groupDebugName.c_str() : "JobSystem::Job");
            NLS_PROFILE_NAMED_SCOPE(scopeName);
            JobWorkerExecutionScope workerExecutionScope(groupHandle);
            std::optional<DisallowJobSyncWaitScope> noSyncWaitScope;
            const bool jobGuaranteesNoSyncWait =
                execution.jobIndex < execution.group->jobSafetyPolicies.size() &&
                execution.group->jobSafetyPolicies[execution.jobIndex] == JobSafetyPolicy::GuaranteedNoSyncWait;
            if (execution.group->safetyPolicy == JobSafetyPolicy::GuaranteedNoSyncWait ||
                jobGuaranteesNoSyncWait)
            {
                noSyncWaitScope.emplace();
            }
            try
            {
                execution.job.function(execution.job.userData);
            }
            catch (...)
            {
                failed = true;
            }

            TryCompleteGroupAfterJob(execution.group, failed);
        }
    }

    void JobQueue::TryCompleteGroupAfterJob(const GroupPtr& group, const bool failed)
    {
        bool shouldComplete = false;
        bool needsCancelCleanup = false;
        bool startedCompleteCallback = false;
        JobFunction completeFunction = nullptr;
        void* completeUserData = nullptr;
        std::vector<std::pair<JobFunction, void*>> cleanupActions;
        {
            std::lock_guard lock(m_mutex);
            if (group->runningJobCount > 0u)
                --group->runningJobCount;
            ++group->completedJobCount;

            if (failed)
            {
                Internal::RecordJobViolation(
                    JobViolationKind::CallbackException,
                    group->id,
                    "Foreground job callback threw an exception.");
                group->state = GroupState::Failed;
                Internal::RecordJobDiagnostic(
                    group->id,
                    group->generation,
                    JobLifecycleState::Failed,
                    group->debugName.c_str(),
                    nullptr,
                    group->dependency.id == 0u ? 0u : 1u);
                m_workAvailable.notify_all();
            }

            const bool hasNoRunningJobs = group->runningJobCount == 0u;
            const bool needsCancellationPath =
                group->state == GroupState::Cancelled ||
                group->state == GroupState::Failed;
            shouldComplete =
                group->completedJobCount >= group->jobs.size() &&
                hasNoRunningJobs &&
                !needsCancellationPath;
            needsCancelCleanup = needsCancellationPath && hasNoRunningJobs;
            if (shouldComplete && group->completeFunction != nullptr)
            {
                group->cleanupConsumed = true;
                group->completeCallbackRunning = true;
                startedCompleteCallback = true;
                completeFunction = group->completeFunction;
                completeUserData = group->completeUserData;
                group->completeFunction = nullptr;
                group->completeUserData = nullptr;
            }
            else if (shouldComplete)
            {
                group->completeCallbackRunning = true;
                startedCompleteCallback = true;
                AppendGroupTerminalCleanup(group, cleanupActions);
            }
        }

        if (needsCancelCleanup)
        {
            RunCancelFunction(group);
            return;
        }

        if (!shouldComplete)
        {
            m_workAvailable.notify_all();
            return;
        }

        if (completeFunction != nullptr)
        {
            try
            {
                NLS_PROFILE_NAMED_SCOPE("JobSystem::CompleteJob");
                completeFunction(completeUserData);
            }
            catch (...)
            {
                Internal::RecordJobViolation(
                    JobViolationKind::CallbackException,
                    group->id,
                    "Foreground job completion callback threw an exception.");
                std::vector<GroupPtr> readyDependents;
                std::vector<GroupPtr> groupsToRetire;
                std::vector<std::pair<JobFunction, void*>> cleanupActions;
                bool notifyCrossQueueDependency = false;
                {
                    std::lock_guard lock(m_mutex);
                    group->state = GroupState::Failed;
                    Internal::RecordJobDiagnostic(
                        group->id,
                        group->generation,
                        JobLifecycleState::Failed,
                        group->debugName.c_str(),
                        nullptr,
                        group->dependency.id == 0u ? 0u : 1u);
                    WakeDependentsForFinishedGroupLocked(
                        group,
                        readyDependents,
                        cleanupActions,
                        groupsToRetire,
                        notifyCrossQueueDependency);
                    AppendGroupTerminalCleanup(group, cleanupActions);
                    QueueGroupForRetirementLocked(group, groupsToRetire);
                }
                for (const auto& [cleanupFunction, cleanupUserData] : cleanupActions)
                {
                    RunUserCallback(
                        cleanupFunction,
                        cleanupUserData,
                        group->id,
                        "Foreground job cleanup callback threw an exception.");
                }
                RetireQueuedGroupsAfterCallbacks(groupsToRetire, notifyCrossQueueDependency);
                NotifyCrossQueueDependencyIfNeeded(notifyCrossQueueDependency);
                return;
            }
        }

        if (startedCompleteCallback && completeFunction != nullptr)
        {
            std::lock_guard lock(m_mutex);
            AppendGroupTerminalCleanup(group, cleanupActions);
        }
        for (const auto& [cleanupFunction, cleanupUserData] : cleanupActions)
        {
            RunUserCallback(
                cleanupFunction,
                cleanupUserData,
                group != nullptr ? group->id : 0u,
                "Foreground job cleanup callback threw an exception.");
        }
        MarkGroupCompleted(group);
    }

    void JobQueue::MarkGroupCompleted(const GroupPtr& group)
    {
        std::vector<GroupPtr> readyDependents;
        std::vector<GroupPtr> groupsToRetire;
        std::vector<std::pair<JobFunction, void*>> cleanupActions;
        bool notifyCrossQueueDependency = false;
        {
            std::lock_guard lock(m_mutex);
            group->state = GroupState::Completed;
            AppendGroupTerminalCleanup(group, cleanupActions);
            Internal::RecordJobDiagnostic(
                group->id,
                group->generation,
                JobLifecycleState::Completed,
                group->debugName.c_str(),
                nullptr,
                group->dependency.id == 0u ? 0u : 1u);
            WakeDependentsForFinishedGroupLocked(
                group,
                readyDependents,
                cleanupActions,
                groupsToRetire,
                notifyCrossQueueDependency);
            AppendGroupTerminalCleanup(group, cleanupActions);
            QueueGroupForRetirementLocked(group, groupsToRetire);
        }

        for (const auto& [cleanupFunction, cleanupUserData] : cleanupActions)
        {
            RunUserCallback(
                cleanupFunction,
                cleanupUserData,
                group != nullptr ? group->id : 0u,
                "Foreground job cleanup callback threw an exception.");
        }
        RetireQueuedGroupsAfterCallbacks(groupsToRetire, notifyCrossQueueDependency);
        NotifyCrossQueueDependencyIfNeeded(notifyCrossQueueDependency);
    }

    void JobQueue::RunCancelFunction(const GroupPtr& group)
    {
        JobFunction cancelFunction = nullptr;
        void* cancelUserData = nullptr;
        std::vector<std::pair<JobFunction, void*>> cleanupActions;
        bool shouldRetire = false;
        {
            std::lock_guard lock(m_mutex);
            if (group == nullptr)
                return;

            const bool shouldRunCancelFunction = group->runCancelOnFailureAfterStart;
            if (!group->cleanupConsumed)
            {
                group->cleanupConsumed = true;
                const bool hasGroupCancelFunction = group->cancelFunction != nullptr;
                if (shouldRunCancelFunction)
                {
                    cancelFunction = group->cancelFunction;
                    cancelUserData = group->cancelUserData;
                }
                group->cancelFunction = nullptr;
                group->cancelUserData = nullptr;
                if (!hasGroupCancelFunction)
                    AppendUnstartedJobCancelCallbacks(group, cleanupActions);
            }
            shouldRetire = group->runningJobCount == 0u;
        }

        if (cancelFunction != nullptr)
            cleanupActions.emplace(cleanupActions.begin(), cancelFunction, cancelUserData);

        std::vector<GroupPtr> readyDependents;
        std::vector<GroupPtr> groupsToRetire;
        bool notifyCrossQueueDependency = false;
        {
            std::lock_guard lock(m_mutex);
            WakeDependentsForFinishedGroupLocked(
                group,
                readyDependents,
                cleanupActions,
                groupsToRetire,
                notifyCrossQueueDependency);
            AppendGroupTerminalCleanup(group, cleanupActions);
            if (shouldRetire)
                QueueGroupForRetirementLocked(group, groupsToRetire);
        }

        for (const auto& [dependentCleanupFunction, dependentCleanupUserData] : cleanupActions)
        {
            RunUserCallback(
                dependentCleanupFunction,
                dependentCleanupUserData,
                group != nullptr ? group->id : 0u,
                "Foreground job cleanup callback threw an exception.");
        }
        RetireQueuedGroupsAfterCallbacks(groupsToRetire, notifyCrossQueueDependency);
        NotifyCrossQueueDependencyIfNeeded(notifyCrossQueueDependency);
    }

    void JobQueue::WakeDependentsForFinishedGroupLocked(
        const GroupPtr& group,
        std::vector<GroupPtr>& readyDependents,
        std::vector<std::pair<JobFunction, void*>>& cleanupActions,
        std::vector<GroupPtr>& groupsToRetire,
        bool& notifyCrossQueueDependency)
    {
        if (group == nullptr)
            return;

        JobCompletionStatus finishedGroupStatus = JobCompletionStatus::Unknown;
        if (group->state == GroupState::Completed)
            finishedGroupStatus = JobCompletionStatus::Succeeded;
        else if (group->state == GroupState::Cancelled)
            finishedGroupStatus = JobCompletionStatus::Cancelled;
        else if (group->state == GroupState::Failed)
            finishedGroupStatus = JobCompletionStatus::Failed;

        const std::vector<std::pair<JobHandle, JobCompletionStatus>> resolvedFinishedGroup =
            finishedGroupStatus != JobCompletionStatus::Unknown
                ? std::vector<std::pair<JobHandle, JobCompletionStatus>>{
                    {{group->id, group->generation}, finishedGroupStatus}}
                : std::vector<std::pair<JobHandle, JobCompletionStatus>>{};

        for (const uint64_t dependentId : group->dependents)
        {
            const auto found = m_groups.find(dependentId);
            if (found == m_groups.end() || found->second == nullptr)
                continue;

            auto dependent = found->second;
            if (dependent->state != GroupState::WaitingForDependencies)
                continue;

            const auto dependencyStatus = GetGroupDependenciesStatusLocked(dependent, resolvedFinishedGroup);
            if (dependencyStatus == JobCompletionStatus::Succeeded)
            {
                readyDependents.push_back(dependent);
                continue;
            }

            if (dependencyStatus == JobCompletionStatus::Pending)
                continue;

            dependent->state = GroupState::Cancelled;
            Internal::RecordJobDiagnostic(
                dependent->id,
                dependent->generation,
                JobLifecycleState::Cancelled,
                dependent->debugName.c_str(),
                nullptr,
                dependent->dependency.id == 0u ? 0u : 1u);
            if (!dependent->cleanupConsumed)
            {
                dependent->cleanupConsumed = true;
                const bool hasGroupCancelFunction = dependent->cancelFunction != nullptr;
                const auto cancelFunction = dependent->cancelFunction;
                const auto cancelUserData = dependent->cancelUserData;
                dependent->cancelFunction = nullptr;
                dependent->cancelUserData = nullptr;
                if (cancelFunction != nullptr)
                    cleanupActions.emplace_back(cancelFunction, cancelUserData);
                if (!hasGroupCancelFunction)
                    AppendUnstartedJobCancelCallbacks(dependent, cleanupActions);
            }
            WakeDependentsForFinishedGroupLocked(
                dependent,
                readyDependents,
                cleanupActions,
                groupsToRetire,
                notifyCrossQueueDependency);
            AppendGroupTerminalCleanup(dependent, cleanupActions);
            QueueGroupForRetirementLocked(dependent, groupsToRetire);
        }

        for (const auto& dependent : readyDependents)
        {
            if (dependent != nullptr && dependent->state == GroupState::WaitingForDependencies)
                EnqueueReadyGroupLocked(dependent, cleanupActions, groupsToRetire, notifyCrossQueueDependency);
        }
    }

    void JobQueue::WorkerLoop(const uint32_t workerIndex)
    {
        const std::string threadName = "Job Worker " + std::to_string(workerIndex);
        NLS_PROFILE_REGISTER_THREAD(threadName.c_str());

        while (true)
        {
            std::optional<JobExecution> execution;
            {
                std::unique_lock lock(m_mutex);
                if (HasExternalDependencyWaiterLocked())
                {
                    m_workAvailable.wait_for(
                        lock,
                        std::chrono::milliseconds(100),
                        [this]
                        {
                                return m_shutdownRequested ||
                                !m_highPriorityQueue.empty() ||
                                !m_normalPriorityQueue.empty() ||
                                m_externalDependencyChanged;
                        });
                    m_externalDependencyChanged = false;
                }
                else
                {
                    m_workAvailable.wait(
                        lock,
                        [this]
                        {
                            return m_shutdownRequested ||
                                !m_highPriorityQueue.empty() ||
                                !m_normalPriorityQueue.empty() ||
                                HasExternalDependencyWaiterLocked();
                        });
                }

                if (m_shutdownRequested && m_highPriorityQueue.empty() && m_normalPriorityQueue.empty())
                    return;

                execution = PopReadyJobLocked();
            }

            if (execution.has_value())
                ExecuteJob(execution.value());
            else
                WakeExternalDependencyReadyGroups();
        }
    }

    std::vector<std::pair<JobFunction, void*>> JobQueue::CancelPendingLocked(
        std::vector<GroupPtr>& groupsToRetire,
        bool& notifyCrossQueueDependency)
    {
        std::vector<std::pair<JobFunction, void*>> cleanupActions;
        std::vector<GroupPtr> groupsToCancel;
        groupsToCancel.reserve(m_groups.size());
        for (const auto& [id, group] : m_groups)
        {
            (void)id;
            if (group != nullptr &&
                group->state != GroupState::Completed &&
                group->state != GroupState::Cancelled &&
                group->state != GroupState::Failed)
            {
                groupsToCancel.push_back(group);
            }
        }

        for (const auto& group : groupsToCancel)
        {
            if (group == nullptr ||
                group->state == GroupState::Completed ||
                group->state == GroupState::Cancelled ||
                group->state == GroupState::Failed)
            {
                continue;
            }

            if (group->completeCallbackRunning)
                continue;

            if (group->runningJobCount == 0u)
            {
                cleanupActions.push_back(CancelGroupLocked(group, cleanupActions));
                AppendGroupTerminalCleanup(group, cleanupActions);
                QueueGroupForRetirementLocked(group, groupsToRetire);
                continue;
            }

            group->state = GroupState::Cancelled;
            Internal::RecordJobDiagnostic(
                group->id,
                group->generation,
                JobLifecycleState::Cancelled,
                group->debugName.c_str(),
                nullptr,
                group->dependency.id == 0u ? 0u : 1u);
        }
        return cleanupActions;
    }
}
