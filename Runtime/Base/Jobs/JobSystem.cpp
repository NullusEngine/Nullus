#include "Jobs/JobSystem.h"

#include <algorithm>
#include <memory>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <unordered_map>
#include <vector>

#include "Jobs/BackgroundJobQueue.h"
#include "Jobs/JobDiagnostics.h"
#include "Jobs/JobQueue.h"
#include "Jobs/JobRange.h"
#include "Jobs/JobSafety.h"

namespace NLS::Base::Jobs
{
namespace
{
    std::mutex g_jobSystemMutex;
    std::condition_variable g_jobSystemLifecycleChanged;
    bool g_initialized = false;
    bool g_shutdownInProgress = false;
    uint32_t g_workerCount = 0u;
    bool g_diagnosticsEnabled = false;
    std::shared_ptr<JobQueue> g_jobQueue;

    struct TerminalStatusKey
    {
        uint64_t id = 0u;
        uint32_t generation = 0u;

        bool operator==(const TerminalStatusKey& other) const
        {
            return id == other.id && generation == other.generation;
        }
    };

    struct TerminalStatusKeyHash
    {
        size_t operator()(const TerminalStatusKey& key) const
        {
            const auto idHash = std::hash<uint64_t>{}(key.id);
            const auto generationHash = std::hash<uint32_t>{}(key.generation);
            return idHash ^ (generationHash + 0x9e3779b97f4a7c15ull + (idHash << 6u) + (idHash >> 2u));
        }
    };

    struct TerminalStatusRecord
    {
        JobCompletionStatus status = JobCompletionStatus::Pending;
        uint32_t retainCount = 0u;
    };

    std::mutex g_terminalStatusMutex;
    std::unordered_map<TerminalStatusKey, TerminalStatusRecord, TerminalStatusKeyHash> g_terminalStatuses;

    thread_local std::vector<JobHandle> g_workerJobHandleStack;

    constexpr uint32_t kMaxResolvedWorkerCount = 64u;

    uint32_t ResolveWorkerCount(const uint32_t requestedWorkerCount)
    {
        uint32_t resolvedWorkerCount = requestedWorkerCount;
        if (requestedWorkerCount == kAutoJobWorkerCount)
        {
            const uint32_t hardwareWorkers = std::thread::hardware_concurrency();
            if (hardwareWorkers <= 1u)
                resolvedWorkerCount = 1u;
            else
                resolvedWorkerCount = std::max(1u, hardwareWorkers - 1u);
        }

        if (resolvedWorkerCount == 0u)
            return 0u;

        return std::min(resolvedWorkerCount, kMaxResolvedWorkerCount);
    }

    bool IsDefaultHandle(const JobHandle handle)
    {
        return handle.id == 0u && handle.generation == 0u;
    }

    std::shared_ptr<JobQueue> GetJobQueueSnapshot()
    {
        std::lock_guard lock(g_jobSystemMutex);
        return g_jobQueue;
    }

    std::shared_ptr<JobQueue> GetAcceptingJobQueueSnapshot()
    {
        std::lock_guard lock(g_jobSystemMutex);
        if (!g_initialized || g_shutdownInProgress)
            return nullptr;
        return g_jobQueue;
    }

    bool IsTerminalStatus(const JobCompletionStatus status)
    {
        return status == JobCompletionStatus::Succeeded ||
            status == JobCompletionStatus::Cancelled ||
            status == JobCompletionStatus::Failed;
    }

    TerminalStatusKey MakeTerminalStatusKey(const JobHandle handle)
    {
        return {handle.id, handle.generation};
    }

    JobCompletionStatus GetTerminalStatus(const JobHandle handle)
    {
        std::lock_guard lock(g_terminalStatusMutex);
        const auto found = g_terminalStatuses.find(MakeTerminalStatusKey(handle));
        if (found == g_terminalStatuses.end())
            return JobCompletionStatus::Unknown;

        return found->second.status;
    }

    JobCompletionStatus GetLiveOrRetiredCompletionStatus(const JobHandle handle)
    {
        if (IsDefaultHandle(handle))
            return JobCompletionStatus::Succeeded;

        if (Internal::IsBackgroundJobHandle(handle))
            return Internal::GetBackgroundJobCompletionStatus(handle);

        auto queue = GetJobQueueSnapshot();
        return queue != nullptr ? queue->GetCompletionStatus(handle) : JobCompletionStatus::Unknown;
    }

    JobHandle ScheduleJobForEachInternal(const JobForEachDesc& desc, const bool pending)
    {
        if (desc.function == nullptr || desc.iterationCount == 0u)
        {
            if (desc.function == nullptr)
            {
                Internal::RecordJobViolation(
                    JobViolationKind::NullCallback,
                    0u,
                    "Parallel-for scheduling received a null callback.");
            }
            return {};
        }
        if (desc.iterationCount > static_cast<uint32_t>(std::numeric_limits<int>::max()))
        {
            Internal::RecordJobViolation(
                JobViolationKind::InvalidHandle,
                desc.iterationCount,
                "Parallel-for iteration count exceeds the native range planner limit.");
            return {};
        }
        if (desc.batchSize > static_cast<uint32_t>(std::numeric_limits<int>::max()))
        {
            Internal::RecordJobViolation(
                JobViolationKind::InvalidHandle,
                desc.batchSize,
                "Parallel-for batch size exceeds the native range planner limit.");
            return {};
        }

        auto queue = GetAcceptingJobQueueSnapshot();
        if (queue == nullptr)
        {
            Internal::RecordJobViolation(
                JobViolationKind::ShutdownSchedulingRejected,
                0u,
                "Parallel-for scheduling was rejected because the JobSystem is not accepting work.");
            return {};
        }

        std::vector<JobScheduleDesc> jobs;
        const uint32_t executionLaneCount = queue->GetWorkerCount() + 1u;
        const int batchSize = static_cast<int>(std::max(1u, desc.batchSize));
        const int rangeJobCount = CalculateJobCountWithMinIndicesPerJob(
            static_cast<int>(desc.iterationCount),
            batchSize,
            static_cast<int>(std::max(1u, executionLaneCount)));
        jobs.reserve(static_cast<size_t>(rangeJobCount));
        struct ForEachPayload
        {
            JobForEachFunction function = nullptr;
            void* userData = nullptr;
            WorkStealingRange* range = nullptr;
            int jobIndex = 0;
        };

        auto range = std::make_shared<WorkStealingRange>();
        InitializeWorkStealingRange(
            *range,
            static_cast<int>(desc.iterationCount),
            batchSize,
            rangeJobCount);

        auto payloads = std::make_shared<std::vector<ForEachPayload>>();
        payloads->reserve(static_cast<size_t>(rangeJobCount));

        for (int jobIndex = 0; jobIndex < rangeJobCount; ++jobIndex)
        {
            payloads->push_back({desc.function, desc.userData, range.get(), jobIndex});
            JobScheduleDesc job;
            job.function = [](void* userData)
            {
                auto* payload = static_cast<ForEachPayload*>(userData);
                int beginIndex = 0;
                int endIndex = 0;
                while (GetWorkStealingRange(*payload->range, payload->jobIndex, beginIndex, endIndex))
                {
                    for (int index = beginIndex; index < endIndex; ++index)
                        payload->function(payload->userData, static_cast<uint32_t>(index));
                }
            };
            job.userData = &payloads->back();
            job.priority = desc.priority;
            job.safetyPolicy = desc.safetyPolicy;
            job.debugName = desc.debugName;
            jobs.push_back(job);
        }

        struct CompletePayload
        {
            std::shared_ptr<std::vector<ForEachPayload>> payloads;
            std::shared_ptr<WorkStealingRange> range;
            JobFunction combineFunction = nullptr;
            JobFunction cancelFunction = nullptr;
            void* combineUserData = nullptr;
        };

        auto* rawCompletePayload = new CompletePayload{
            std::move(payloads),
            std::move(range),
            desc.combineFunction,
            desc.cancelFunction,
            desc.userData};
        JobFunction completeFunction = [](void* userData)
        {
            std::unique_ptr<CompletePayload> payload(static_cast<CompletePayload*>(userData));
            if (payload->combineFunction != nullptr)
                payload->combineFunction(payload->combineUserData);
        };
        JobFunction cancelFunction = [](void* userData)
        {
            std::unique_ptr<CompletePayload> payload(static_cast<CompletePayload*>(userData));
            if (payload->cancelFunction != nullptr)
                payload->cancelFunction(payload->combineUserData);
        };

        if (pending)
        {
            const auto handle = queue->CreatePendingJobs(
                std::move(jobs),
                desc.dependency,
                completeFunction,
                rawCompletePayload,
                cancelFunction,
                rawCompletePayload,
                true,
                desc.priority,
                desc.safetyPolicy,
                desc.debugName);
            if (handle.id == 0u)
                cancelFunction(rawCompletePayload);
            return handle;
        }

        const auto handle = queue->ScheduleJobs(
            std::move(jobs),
            desc.dependency,
            completeFunction,
            rawCompletePayload,
            cancelFunction,
            rawCompletePayload,
            true,
            desc.priority,
            desc.safetyPolicy,
            desc.debugName);
        if (handle.id == 0u)
            cancelFunction(rawCompletePayload);
        return handle;
    }

}

bool TryInitializeJobSystem(const JobSystemConfig& config)
{
    std::lock_guard lock(g_jobSystemMutex);
    if (g_shutdownInProgress)
        return false;
    if (g_initialized)
        return false;

    g_diagnosticsEnabled = config.enableDiagnostics;
    const uint32_t resolvedWorkerCount = ResolveWorkerCount(config.workerCount);
    auto jobQueue = std::make_shared<JobQueue>();
    if (!jobQueue->Start(resolvedWorkerCount))
    {
        g_diagnosticsEnabled = false;
        return false;
    }
    if (!Internal::StartBackgroundJobQueue(config.backgroundWorkerCount))
    {
        jobQueue->Shutdown(JobSystemShutdownMode::Immediate);
        g_diagnosticsEnabled = false;
        return false;
    }
    g_workerCount = resolvedWorkerCount;
    g_jobQueue = std::move(jobQueue);
    g_initialized = true;
    Internal::SetJobDiagnosticsRuntimeState(true, g_workerCount, g_diagnosticsEnabled);
    return true;
}

bool InitializeJobSystem(const JobSystemConfig& config)
{
    if (TryInitializeJobSystem(config))
        return true;

    std::lock_guard lock(g_jobSystemMutex);
    return g_initialized && !g_shutdownInProgress;
}

void ShutdownJobSystem(const JobSystemShutdownMode mode)
{
    if (Internal::IsExecutingJobWorkerThread())
    {
        Internal::RecordJobViolation(
            JobViolationKind::ShutdownSchedulingRejected,
            0u,
            "ShutdownJobSystem cannot be called from a JobSystem worker callback.");
        return;
    }

    std::shared_ptr<JobQueue> queue;
    bool diagnosticsWereEnabled = false;
    {
        std::unique_lock lock(g_jobSystemMutex);
        if (g_shutdownInProgress)
        {
            g_jobSystemLifecycleChanged.wait(
                lock,
                []
                {
                    return !g_shutdownInProgress;
                });
            return;
        }
        g_shutdownInProgress = true;
        queue = g_jobQueue;
        diagnosticsWereEnabled = g_diagnosticsEnabled;
    }

    if (queue != nullptr)
        queue->StopAcceptingWork();
    Internal::StopAcceptingBackgroundJobQueue();

    if (mode == JobSystemShutdownMode::DrainAcceptedWork && queue != nullptr)
        queue->KickAllPending();

    Internal::ShutdownBackgroundJobQueue(mode);
    if (queue != nullptr)
        queue->Shutdown(mode);
    {
        std::lock_guard lock(g_jobSystemMutex);
        g_initialized = false;
        g_workerCount = 0u;
        g_diagnosticsEnabled = false;
        g_jobQueue.reset();
        g_shutdownInProgress = false;
    }
    Internal::SetJobDiagnosticsRuntimeState(false, 0u, diagnosticsWereEnabled);
    g_jobSystemLifecycleChanged.notify_all();
}

bool IsJobSystemInitialized()
{
    std::lock_guard lock(g_jobSystemMutex);
    return g_initialized && !g_shutdownInProgress;
}

uint32_t GetJobWorkerCount()
{
    std::lock_guard lock(g_jobSystemMutex);
    return g_workerCount;
}

bool ExecuteOneJobQueueJob()
{
    auto queue = GetJobQueueSnapshot();
    return queue != nullptr && queue->ExecuteOneJob();
}

JobHandle ScheduleJob(const JobScheduleDesc& desc)
{
    auto queue = GetAcceptingJobQueueSnapshot();
    if (queue == nullptr)
    {
        Internal::RecordJobViolation(
            JobViolationKind::ShutdownSchedulingRejected,
            0u,
            "Job scheduling was rejected because the JobSystem is not accepting work.");
        return {};
    }

    return queue->ScheduleJob(desc);
}

JobHandle ScheduleJobDepends(const JobScheduleDesc& desc, const JobHandle dependency)
{
    auto queue = GetAcceptingJobQueueSnapshot();
    if (queue == nullptr)
    {
        Internal::RecordJobViolation(
            JobViolationKind::ShutdownSchedulingRejected,
            0u,
            "Dependent job scheduling was rejected because the JobSystem is not accepting work.");
        return {};
    }

    return queue->ScheduleJobDepends(desc, dependency);
}

JobHandle ScheduleDifferentJobsConcurrent(
    const JobScheduleDesc* jobs,
    const size_t jobCount,
    const JobHandle dependency)
{
    if (jobs == nullptr || jobCount == 0u)
        return {};

    auto queue = GetAcceptingJobQueueSnapshot();
    if (queue == nullptr)
    {
        Internal::RecordJobViolation(
            JobViolationKind::ShutdownSchedulingRejected,
            0u,
            "Concurrent job scheduling was rejected because the JobSystem is not accepting work.");
        return {};
    }

    std::vector<JobScheduleDesc> jobDescs(jobs, jobs + jobCount);
    const bool hasHighPriorityJob = std::any_of(
        jobDescs.begin(),
        jobDescs.end(),
        [](const JobScheduleDesc& job)
        {
            return job.priority == JobPriority::High;
        });
    const bool allJobsGuaranteeNoSyncWait = std::all_of(
        jobDescs.begin(),
        jobDescs.end(),
        [](const JobScheduleDesc& job)
        {
            return job.safetyPolicy == JobSafetyPolicy::GuaranteedNoSyncWait;
        });

    const auto handle = queue->ScheduleJobs(
        std::move(jobDescs),
        dependency,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        false,
        hasHighPriorityJob ? JobPriority::High : JobPriority::Normal,
        allJobsGuaranteeNoSyncWait ? JobSafetyPolicy::GuaranteedNoSyncWait : JobSafetyPolicy::MaySyncWait,
        nullptr);

    if (handle.id == 0u)
        return {};

    return handle;
}

JobHandle ScheduleJobForEach(const JobForEachDesc& desc)
{
    return ScheduleJobForEachInternal(desc, false);
}

void Complete(JobHandle& handle)
{
    CompleteNoClear(handle);
    const auto status = Internal::GetJobCompletionStatus(handle);
    if (IsDefaultHandle(handle) || IsTerminalStatus(status) || status == JobCompletionStatus::Unknown)
        handle = {};
}

void CompleteNoClear(const JobHandle handle)
{
    const auto currentStatus = Internal::GetJobCompletionStatus(handle);
    if (!IsDefaultHandle(handle) &&
        IsJobSyncWaitDisallowedForCurrentThread() &&
        currentStatus == JobCompletionStatus::Pending)
    {
        Internal::RecordJobViolation(
            JobViolationKind::SyncWaitDisallowed,
            handle.id,
            "Synchronous job completion is disallowed in the current scope.");
        return;
    }

    if (Internal::IsCurrentJobWorkerHandle(handle))
    {
        Internal::RecordJobViolation(
            JobViolationKind::SyncWaitDisallowed,
            handle.id,
            "A job callback cannot synchronously wait for its own job handle.");
        return;
    }

    if (Internal::IsBackgroundJobHandle(handle))
    {
        Internal::CompleteBackgroundJob(handle);
        return;
    }

    auto queue = GetJobQueueSnapshot();
    if (queue != nullptr)
    {
        if (!queue->IsKnownHandle(handle))
        {
            Internal::RecordJobViolation(
                JobViolationKind::StaleHandle,
                handle.id,
                "Completion received an unknown or stale foreground job handle.");
        }
        queue->Complete(handle);
    }
    else if (!IsDefaultHandle(handle))
    {
        Internal::RecordJobViolation(
            JobViolationKind::StaleHandle,
            handle.id,
            "Completion received a foreground job handle after queue shutdown.");
    }
}

void CompleteAll(JobHandle* handles, const size_t handleCount)
{
    if (handles == nullptr)
        return;

    for (size_t index = 0u; index < handleCount; ++index)
        Complete(handles[index]);
}

bool IsCompleted(const JobHandle handle)
{
    if (IsDefaultHandle(handle))
        return true;

    if (Internal::IsBackgroundJobHandle(handle))
        return Internal::IsBackgroundJobCompleted(handle);

    auto queue = GetJobQueueSnapshot();
    if (queue == nullptr)
    {
        Internal::RecordJobViolation(
            JobViolationKind::StaleHandle,
            handle.id,
            "Completion query received a foreground job handle after queue shutdown.");
        return true;
    }

    if (!queue->IsKnownHandle(handle))
    {
        Internal::RecordJobViolation(
            JobViolationKind::StaleHandle,
            handle.id,
            "Completion query received an unknown or stale foreground job handle.");
    }
    Internal::KickPendingJobForBatch(handle);
    return queue->IsCompleted(handle);
}

void ClearWithoutSync(JobHandle& handle)
{
    if (!IsDefaultHandle(handle))
    {
        Internal::RecordJobViolation(
            JobViolationKind::ClearedWithoutSync,
            handle.id,
            "Job handle was cleared without waiting for completion.");
    }

    if (Internal::IsBackgroundJobHandle(handle))
    {
        Internal::ClearBackgroundJob(handle);
        handle = {};
        return;
    }

    auto queue = GetJobQueueSnapshot();
    if (queue != nullptr)
        queue->ClearWithoutSync(handle);
    handle = {};
}

bool HasBeenSynced(const JobHandle handle)
{
    if (IsDefaultHandle(handle))
        return true;

    return IsTerminalStatus(Internal::GetJobCompletionStatus(handle));
}

#if defined(NLS_ENABLE_TEST_HOOKS)
void ResetJobSystemForTesting()
{
    std::shared_ptr<JobQueue> queue;
    {
        std::lock_guard lock(g_jobSystemMutex);
        queue = std::move(g_jobQueue);
        g_initialized = false;
        g_workerCount = 0u;
        g_diagnosticsEnabled = false;
        g_shutdownInProgress = false;
    }
    if (queue != nullptr)
        queue->StopAcceptingWork();
    Internal::StopAcceptingBackgroundJobQueue();

    if (queue != nullptr)
        queue->Shutdown(JobSystemShutdownMode::Immediate);
    Internal::ShutdownBackgroundJobQueue(JobSystemShutdownMode::Immediate);
    Internal::ClearBackgroundJobRetiredHistory();
    Internal::ClearJobTerminalStatusesForTesting();

    std::lock_guard lock(g_jobSystemMutex);
    g_jobQueue.reset();
    g_jobSystemLifecycleChanged.notify_all();
}
#endif

namespace Internal
{
    bool IsKnownJobHandle(const JobHandle handle)
    {
        if (IsDefaultHandle(handle))
            return true;

        if (IsBackgroundJobHandle(handle))
            return IsKnownBackgroundJobHandle(handle);

        auto queue = GetJobQueueSnapshot();
        return queue != nullptr && queue->IsKnownHandle(handle);
    }

    JobCompletionStatus GetJobCompletionStatus(const JobHandle handle)
    {
        if (IsDefaultHandle(handle))
            return JobCompletionStatus::Succeeded;

        if (IsBackgroundJobHandle(handle))
        {
            const auto status = GetBackgroundJobCompletionStatus(handle);
            return status != JobCompletionStatus::Unknown
                ? status
                : GetTerminalStatus(handle);
        }

        const auto status = GetLiveOrRetiredCompletionStatus(handle);
        if (status != JobCompletionStatus::Unknown)
            return status;

        return GetTerminalStatus(handle);
    }

    JobCompletionStatus RetainJobCompletionStatus(const JobHandle handle, bool& retained)
    {
        retained = false;
        if (IsDefaultHandle(handle))
            return JobCompletionStatus::Succeeded;

        {
            std::lock_guard lock(g_terminalStatusMutex);
            const auto key = MakeTerminalStatusKey(handle);
            auto [iter, inserted] = g_terminalStatuses.try_emplace(key);
            ++iter->second.retainCount;
            retained = true;
            if (IsTerminalStatus(iter->second.status))
                return iter->second.status;
        }

        const auto status = GetLiveOrRetiredCompletionStatus(handle);
        {
            std::lock_guard lock(g_terminalStatusMutex);
            const auto key = MakeTerminalStatusKey(handle);
            const auto found = g_terminalStatuses.find(key);
            if (found == g_terminalStatuses.end())
            {
                retained = false;
                return status;
            }

            if (IsTerminalStatus(status))
            {
                found->second.status = status;
                return status;
            }

            if (IsTerminalStatus(found->second.status))
                return found->second.status;

            if (status == JobCompletionStatus::Unknown)
            {
                if (found->second.retainCount > 0u)
                    --found->second.retainCount;
                if (found->second.retainCount == 0u)
                    g_terminalStatuses.erase(found);
                retained = false;
                return JobCompletionStatus::Unknown;
            }
        }

        return status;
    }

    void ReleaseJobCompletionStatus(const JobHandle handle)
    {
        if (IsDefaultHandle(handle))
            return;

        std::lock_guard lock(g_terminalStatusMutex);
        const auto key = MakeTerminalStatusKey(handle);
        const auto found = g_terminalStatuses.find(key);
        if (found == g_terminalStatuses.end())
            return;

        if (found->second.retainCount > 0u)
            --found->second.retainCount;
        if (found->second.retainCount == 0u)
            g_terminalStatuses.erase(found);
    }

    bool RecordJobTerminalStatus(const JobHandle handle, const JobCompletionStatus status)
    {
        if (IsDefaultHandle(handle))
            return false;

        {
            std::lock_guard lock(g_terminalStatusMutex);
            const auto key = MakeTerminalStatusKey(handle);
            const auto found = g_terminalStatuses.find(key);
            if (found != g_terminalStatuses.end())
            {
                found->second.status = status;
                return found->second.retainCount > 0u;
            }
        }
        return false;
    }

    void ClearJobTerminalStatusesForTesting()
    {
        std::lock_guard lock(g_terminalStatusMutex);
        g_terminalStatuses.clear();
    }

    JobHandle CreatePendingJobForBatch(const JobScheduleDesc& desc)
    {
        auto queue = GetAcceptingJobQueueSnapshot();
        if (queue == nullptr)
            return {};

        return queue->CreatePendingJob(desc);
    }

    JobHandle CreatePendingForEachJobForBatch(const JobForEachDesc& desc)
    {
        return ScheduleJobForEachInternal(desc, true);
    }

    JobHandle ScheduleMultiDependencyFence(
        const JobHandle* dependencies,
        const size_t dependencyCount)
    {
        if (dependencies == nullptr || dependencyCount == 0u)
            return {};

        auto queue = GetAcceptingJobQueueSnapshot();
        if (queue == nullptr)
            return {};

        return queue->ScheduleJobsMultiDepends(
            {},
            std::vector<JobHandle>(dependencies, dependencies + dependencyCount),
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            false,
            JobPriority::Normal,
            JobSafetyPolicy::MaySyncWait,
            "JobBatchDispatcher::MultiDependencyFence");
    }

    void KickPendingJobForBatch(const JobHandle handle)
    {
        auto queue = GetJobQueueSnapshot();
        if (queue != nullptr)
            queue->KickPending(handle);
    }

    void KickAllPendingForegroundJobs()
    {
        auto queue = GetJobQueueSnapshot();
        if (queue != nullptr)
            queue->KickAllPending();
    }

    bool ExecuteForegroundJobForWait(
        const JobHandle waitedHandle,
        const bool allowNoSyncWaitOpportunisticWork)
    {
        auto queue = GetJobQueueSnapshot();
        return queue != nullptr &&
            queue->ExecuteOneJobForWait(waitedHandle, {}, allowNoSyncWaitOpportunisticWork);
    }

    void NotifyForegroundDependencyChanged()
    {
        auto queue = GetJobQueueSnapshot();
        if (queue != nullptr)
            queue->NotifyExternalDependencyChanged();
    }

    void EnterJobWorkerExecution(const JobHandle handle)
    {
        g_workerJobHandleStack.push_back(handle);
    }

    void ExitJobWorkerExecution()
    {
        if (!g_workerJobHandleStack.empty())
            g_workerJobHandleStack.pop_back();
    }

    bool IsExecutingJobWorkerThread()
    {
        return !g_workerJobHandleStack.empty();
    }

    bool IsExecutingBackgroundJobWorkerThread()
    {
        return std::any_of(
            g_workerJobHandleStack.begin(),
            g_workerJobHandleStack.end(),
            [](const JobHandle currentHandle)
            {
                return IsBackgroundJobHandle(currentHandle);
            });
    }

    bool IsCurrentJobWorkerHandle(const JobHandle handle)
    {
        return std::any_of(
            g_workerJobHandleStack.begin(),
            g_workerJobHandleStack.end(),
            [handle](const JobHandle currentHandle)
            {
                return currentHandle.id == handle.id &&
                    currentHandle.generation == handle.generation;
            });
    }

    JobHandle GetCurrentJobWorkerHandle()
    {
        return !g_workerJobHandleStack.empty() ? g_workerJobHandleStack.back() : JobHandle{};
    }

    bool DoesJobDependencyChainContainCurrentWorker(const JobHandle handle)
    {
        std::vector<JobHandle> stack;
        std::vector<JobHandle> visited;
        stack.push_back(handle);

        while (!stack.empty())
        {
            const JobHandle current = stack.back();
            stack.pop_back();

            if (IsDefaultHandle(current))
                continue;
            if (IsCurrentJobWorkerHandle(current))
                return true;
            if (std::any_of(
                    visited.begin(),
                    visited.end(),
                    [current](const JobHandle existing)
                    {
                        return existing.id == current.id && existing.generation == current.generation;
                    }))
            {
                continue;
            }
            visited.push_back(current);

            const auto status = GetJobCompletionStatus(current);
            if (IsTerminalStatus(status))
                continue;

            std::vector<JobHandle> dependencies;
            if (IsBackgroundJobHandle(current))
            {
                dependencies = CollectBackgroundJobDependencies(current);
            }
            else
            {
                auto queue = GetJobQueueSnapshot();
                if (queue != nullptr)
                    dependencies = queue->CollectDirectDependencies(current);
            }

            for (const JobHandle dependency : dependencies)
                stack.push_back(dependency);
        }

        return false;
    }
}
}
