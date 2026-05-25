#pragma once

#include "Jobs/JobRange.h"
#include "Jobs/JobTypes.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace NLS::Base::Jobs
{
    NLS_BASE_API bool InitializeJobSystem(const JobSystemConfig& config = {});
    NLS_BASE_API bool TryInitializeJobSystem(const JobSystemConfig& config = {});
    NLS_BASE_API void ShutdownJobSystem(JobSystemShutdownMode mode = JobSystemShutdownMode::DrainAcceptedWork);
    NLS_BASE_API bool IsJobSystemInitialized();
    NLS_BASE_API uint32_t GetJobWorkerCount();

    NLS_BASE_API bool ExecuteOneJobQueueJob();

    NLS_BASE_API JobHandle ScheduleJob(const JobScheduleDesc& desc);
    NLS_BASE_API JobHandle ScheduleJobDepends(const JobScheduleDesc& desc, JobHandle dependency);
    NLS_BASE_API JobHandle ScheduleDifferentJobsConcurrent(
        const JobScheduleDesc* jobs,
        size_t jobCount,
        JobHandle dependency = {});
    NLS_BASE_API JobHandle ScheduleJobForEach(const JobForEachDesc& desc);

    struct IJob
    {
    };

    struct IJobParallelFor
    {
    };

    struct JobScheduleOptions
    {
        JobHandle dependency;
        JobPriority priority = JobPriority::Normal;
        JobSafetyPolicy safetyPolicy = JobSafetyPolicy::MaySyncWait;
        const char* debugName = nullptr;
    };

    struct JobParallelForScheduleOptions
    {
        JobHandle dependency;
        uint32_t batchSize = 1u;
        JobPriority priority = JobPriority::Normal;
        JobSafetyPolicy safetyPolicy = JobSafetyPolicy::MaySyncWait;
        const char* debugName = nullptr;
    };

    template<typename TJob>
    JobHandle Schedule(TJob job, const JobScheduleOptions& options = {})
    {
        static_assert(std::is_base_of_v<IJob, TJob>, "Schedule(job) expects a type derived from IJob.");
        static_assert(std::is_invocable_v<decltype(&TJob::Execute), TJob&>, "IJob types must expose void Execute().");

        using JobT = std::decay_t<TJob>;
        auto payload = std::make_unique<JobT>(std::move(job));
        JobScheduleDesc desc;
        desc.function = [](void* userData)
        {
            std::unique_ptr<JobT> payload(static_cast<JobT*>(userData));
            payload->Execute();
        };
        desc.userData = payload.get();
        desc.cancelFunction = [](void* userData)
        {
            delete static_cast<JobT*>(userData);
        };
        desc.cancelUserData = payload.get();
        desc.dependency = options.dependency;
        desc.priority = options.priority;
        desc.safetyPolicy = options.safetyPolicy;
        desc.debugName = options.debugName;

        JobHandle handle = ScheduleJob(desc);
        if (handle.id == 0u)
            return {};

        (void)payload.release();
        return handle;
    }

    template<typename TJob>
    JobHandle ScheduleParallelFor(
        TJob job,
        const uint32_t iterationCount,
        const JobParallelForScheduleOptions& options = {})
    {
        static_assert(
            std::is_base_of_v<IJobParallelFor, TJob>,
            "ScheduleParallelFor(job) expects a type derived from IJobParallelFor.");
        static_assert(
            std::is_invocable_v<decltype(&TJob::Execute), TJob&, uint32_t>,
            "IJobParallelFor types must expose void Execute(uint32_t index).");

        using JobT = std::decay_t<TJob>;
        static_assert(
            std::is_copy_constructible_v<JobT>,
            "IJobParallelFor types must be copy constructible so each scheduled shard owns its job data.");

        if (iterationCount == 0u)
            return {};
        if (iterationCount > static_cast<uint32_t>(std::numeric_limits<int>::max()))
            return {};
        if (options.batchSize > static_cast<uint32_t>(std::numeric_limits<int>::max()))
            return {};

        const uint32_t effectiveBatchSize = std::max(1u, options.batchSize);
        const uint32_t executionLaneCount = std::max(1u, GetJobWorkerCount() + 1u);
        const int shardCount = CalculateJobCountWithMinIndicesPerJob(
            static_cast<int>(iterationCount),
            static_cast<int>(effectiveBatchSize),
            static_cast<int>(executionLaneCount));

        auto range = std::make_shared<WorkStealingRange>();
        InitializeWorkStealingRange(
            *range,
            static_cast<int>(iterationCount),
            static_cast<int>(effectiveBatchSize),
            shardCount);

        struct ParallelShardPayload
        {
            JobT job;
            std::shared_ptr<WorkStealingRange> range;
            int jobIndex = 0;
        };

        std::vector<std::unique_ptr<ParallelShardPayload>> payloads;
        payloads.reserve(static_cast<size_t>(shardCount));
        for (int shardIndex = 0; shardIndex < shardCount; ++shardIndex)
        {
            payloads.push_back(std::make_unique<ParallelShardPayload>(ParallelShardPayload{
                job,
                range,
                shardIndex}));
        }

        std::vector<JobScheduleDesc> jobs;
        jobs.reserve(payloads.size());
        for (const auto& payload : payloads)
        {
            JobScheduleDesc desc;
            desc.function = [](void* userData)
            {
                std::unique_ptr<ParallelShardPayload> payload(static_cast<ParallelShardPayload*>(userData));
                int beginIndex = 0;
                int endIndex = 0;
                while (GetWorkStealingRange(*payload->range, payload->jobIndex, beginIndex, endIndex))
                {
                    for (int index = beginIndex; index < endIndex; ++index)
                        payload->job.Execute(static_cast<uint32_t>(index));
                }
            };
            desc.userData = payload.get();
            desc.cancelFunction = [](void* userData)
            {
                delete static_cast<ParallelShardPayload*>(userData);
            };
            desc.cancelUserData = payload.get();
            desc.priority = options.priority;
            desc.safetyPolicy = options.safetyPolicy;
            desc.debugName = options.debugName;
            jobs.push_back(desc);
        }

        JobHandle handle = ScheduleDifferentJobsConcurrent(
            jobs.data(),
            jobs.size(),
            options.dependency);
        if (handle.id == 0u)
        {
            return {};
        }

        for (auto& payload : payloads)
            (void)payload.release();
        return handle;
    }

    NLS_BASE_API void Complete(JobHandle& handle);
    NLS_BASE_API void CompleteNoClear(JobHandle handle);
    NLS_BASE_API void CompleteAll(JobHandle* handles, size_t handleCount);
    NLS_BASE_API bool IsCompleted(JobHandle handle);
    NLS_BASE_API void ClearWithoutSync(JobHandle& handle);
    NLS_BASE_API bool HasBeenSynced(JobHandle handle);

#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS_BASE_API void ResetJobSystemForTesting();
#endif

    namespace Internal
    {
        NLS_BASE_API JobHandle CreatePendingJobForBatch(const JobScheduleDesc& desc);
        NLS_BASE_API JobHandle CreatePendingForEachJobForBatch(const JobForEachDesc& desc);
        NLS_BASE_API JobHandle ScheduleMultiDependencyFence(
            const JobHandle* dependencies,
            size_t dependencyCount);
        NLS_BASE_API void KickPendingJobForBatch(JobHandle handle);
        NLS_BASE_API bool IsKnownJobHandle(JobHandle handle);
        NLS_BASE_API JobCompletionStatus GetJobCompletionStatus(JobHandle handle);
        NLS_BASE_API JobCompletionStatus RetainJobCompletionStatus(JobHandle handle, bool& retained);
        NLS_BASE_API void ReleaseJobCompletionStatus(JobHandle handle);
        NLS_BASE_API bool RecordJobTerminalStatus(JobHandle handle, JobCompletionStatus status);
        NLS_BASE_API void ClearJobTerminalStatusesForTesting();
        NLS_BASE_API void KickAllPendingForegroundJobs();
        NLS_BASE_API bool ExecuteForegroundJobForWait(
            JobHandle waitedHandle,
            bool allowNoSyncWaitOpportunisticWork = true);
        NLS_BASE_API void NotifyForegroundDependencyChanged();
        NLS_BASE_API void EnterJobWorkerExecution(JobHandle handle);
        NLS_BASE_API void ExitJobWorkerExecution();
        NLS_BASE_API bool IsExecutingJobWorkerThread();
        NLS_BASE_API bool IsExecutingBackgroundJobWorkerThread();
        NLS_BASE_API bool IsCurrentJobWorkerHandle(JobHandle handle);
        NLS_BASE_API JobHandle GetCurrentJobWorkerHandle();
        NLS_BASE_API bool DoesJobDependencyChainContainCurrentWorker(JobHandle handle);
    }
}
