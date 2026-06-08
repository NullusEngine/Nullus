#include <gtest/gtest.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Jobs/JobBatchDispatcher.h"
#include "Jobs/JobRange.h"
#include "Jobs/JobDiagnostics.h"
#include "Jobs/JobSystem.h"
#include "Profiling/Profiler.h"

namespace
{
    class JobSystemParallelTests : public ::testing::Test
    {
    protected:
        void TearDown() override
        {
            NLS::Base::Jobs::ShutdownJobSystem();
#if defined(NLS_ENABLE_TEST_HOOKS)
            NLS::Base::Jobs::ResetJobSystemForTesting();
#endif
        }
    };

    struct CounterJobData
    {
        std::atomic<int>* counter = nullptr;
    };

    void IncrementCounterJob(void* userData)
    {
        auto* data = static_cast<CounterJobData*>(userData);
        data->counter->fetch_add(1, std::memory_order_acq_rel);
    }

    struct ForEachData
    {
        std::vector<int>* visits = nullptr;
        std::atomic<int>* totalVisits = nullptr;
    };

    void VisitIndexJob(void* userData, const uint32_t index)
    {
        auto* data = static_cast<ForEachData*>(userData);
        (*data->visits)[index] += 1;
        data->totalVisits->fetch_add(1, std::memory_order_acq_rel);
    }

    struct AtomicForEachData
    {
        std::vector<std::atomic<int>>* visits = nullptr;
        std::atomic<int>* totalVisits = nullptr;
    };

    void VisitIndexAtomicJob(void* userData, const uint32_t index)
    {
        auto* data = static_cast<AtomicForEachData*>(userData);
        (*data->visits)[index].fetch_add(1, std::memory_order_acq_rel);
        data->totalVisits->fetch_add(1, std::memory_order_acq_rel);
    }

    struct CombineData
    {
        std::vector<int>* visits = nullptr;
        std::atomic<int>* combineCount = nullptr;
        std::atomic<int>* totalAtCombine = nullptr;
    };

    void VisitIndexForCombineJob(void* userData, const uint32_t index)
    {
        auto* data = static_cast<CombineData*>(userData);
        (*data->visits)[index] += 1;
    }

    void CombineJob(void* userData)
    {
        auto* data = static_cast<CombineData*>(userData);
        int total = 0;
        for (const int visit : *data->visits)
            total += visit;

        data->totalAtCombine->store(total, std::memory_order_release);
        data->combineCount->fetch_add(1, std::memory_order_acq_rel);
    }

    struct BlockingForEachData
    {
        std::atomic<int>* started = nullptr;
        std::atomic<bool>* release = nullptr;
    };

    struct BlockingCounterJobData
    {
        std::atomic<int>* started = nullptr;
        std::atomic<int>* finished = nullptr;
        std::atomic<bool>* release = nullptr;
    };

    void BlockingVisitIndex(void* userData, uint32_t)
    {
        auto* data = static_cast<BlockingForEachData*>(userData);
        data->started->fetch_add(1, std::memory_order_acq_rel);
        while (!data->release->load(std::memory_order_acquire))
            std::this_thread::yield();
    }

    void BlockingCounterJob(void* userData)
    {
        auto* data = static_cast<BlockingCounterJobData*>(userData);
        data->started->fetch_add(1, std::memory_order_acq_rel);
        while (!data->release->load(std::memory_order_acquire))
            std::this_thread::yield();
        data->finished->fetch_add(1, std::memory_order_acq_rel);
    }

    struct ThrowingForEachData
    {
        std::atomic<int>* started = nullptr;
        std::atomic<int>* afterThrowerReturned = nullptr;
        std::atomic<bool>* allowThrow = nullptr;
        std::atomic<bool>* release = nullptr;
    };

    void ThrowingOrBlockingVisitIndex(void* userData, const uint32_t index)
    {
        auto* data = static_cast<ThrowingForEachData*>(userData);
        data->started->fetch_add(1, std::memory_order_acq_rel);
        if (index == 0u)
        {
            while (!data->allowThrow->load(std::memory_order_acquire))
                std::this_thread::yield();
            throw std::runtime_error("expected parallel-for shard failure");
        }

        while (!data->release->load(std::memory_order_acquire))
            std::this_thread::yield();
        data->afterThrowerReturned->fetch_add(1, std::memory_order_acq_rel);
    }

    struct ThrowingAndBlockingForEachData
    {
        std::atomic<int>* started = nullptr;
        std::atomic<int>* blockerFinished = nullptr;
        std::atomic<bool>* release = nullptr;
    };

    void ThrowingDrainJob(void* userData)
    {
        auto* data = static_cast<ThrowingAndBlockingForEachData*>(userData);
        data->started->fetch_add(1, std::memory_order_acq_rel);
        while (data->started->load(std::memory_order_acquire) < 2)
            std::this_thread::yield();
        throw std::runtime_error("expected drain-shutdown job failure");
    }

    void BlockingDrainJob(void* userData)
    {
        auto* data = static_cast<ThrowingAndBlockingForEachData*>(userData);
        data->started->fetch_add(1, std::memory_order_acq_rel);
        while (!data->release->load(std::memory_order_acquire))
            std::this_thread::yield();
        data->blockerFinished->fetch_add(1, std::memory_order_acq_rel);
    }

    void ThrowingJob(void*)
    {
        throw std::runtime_error("expected multi-dependency failure");
    }

    void CountCancel(void* userData)
    {
        auto* counter = static_cast<std::atomic<int>*>(userData);
        counter->fetch_add(1, std::memory_order_acq_rel);
    }

    struct ReentrantCancelData
    {
        std::atomic<int>* counter = nullptr;
        std::atomic<bool>* returned = nullptr;
    };

    void ReentrantScheduleFromCancel(void* userData)
    {
        auto* data = static_cast<ReentrantCancelData*>(userData);
        CounterJobData jobData{data->counter};
        NLS::Base::Jobs::JobScheduleDesc desc;
        desc.function = IncrementCounterJob;
        desc.userData = &jobData;
        auto handle = NLS::Base::Jobs::ScheduleJob(desc);
        NLS::Base::Jobs::Complete(handle);
        data->returned->store(true, std::memory_order_release);
    }

    struct BlockingCombineData
    {
        std::atomic<int>* combineStarted = nullptr;
        std::atomic<bool>* releaseCombine = nullptr;
        std::atomic<int>* combineReturned = nullptr;
    };

    struct CountedPayload
    {
        std::atomic<int>* destroyed = nullptr;
        std::atomic<int>* cancelCount = nullptr;
        bool destroyOnCancel = false;
    };

    void NoOpForEach(void*, uint32_t)
    {
    }

    void BlockingCombine(void* userData)
    {
        auto* data = static_cast<BlockingCombineData*>(userData);
        data->combineStarted->fetch_add(1, std::memory_order_acq_rel);
        while (!data->releaseCombine->load(std::memory_order_acquire))
            std::this_thread::yield();
        data->combineReturned->fetch_add(1, std::memory_order_acq_rel);
    }

    void CountAndMaybeDestroyCancel(void* userData)
    {
        auto* payload = static_cast<CountedPayload*>(userData);
        payload->cancelCount->fetch_add(1, std::memory_order_acq_rel);
        if (payload->destroyOnCancel)
        {
            payload->destroyed->fetch_add(1, std::memory_order_acq_rel);
            delete payload;
        }
    }

    uint32_t PackTestWorkStealingSlot(const int start, const int end)
    {
        return (static_cast<uint32_t>(end) << 16u) | static_cast<uint32_t>(start);
    }

    struct RecordedProfilerScope
    {
        std::string phase;
        std::string name;
    };

    class RecordingProfilerDestination final : public NLS::Base::Profiling::IProfilerDestination
    {
    public:
        void BeginScope(const NLS::Base::Profiling::ProfilerScopeEvent& event) override
        {
            std::lock_guard lock(mutex);
            events.push_back({"begin", event.name});
        }

        void EndScope(const NLS::Base::Profiling::ProfilerScopeEvent& event) override
        {
            std::lock_guard lock(mutex);
            events.push_back({"end", event.name});
        }

        NLS::Base::Profiling::ProfilerDestinationState GetState() const override
        {
            return {
                NLS::Base::Profiling::ProfilerDestinationId::Test,
                true,
                NLS::Base::Profiling::ProfilerAvailability::Available,
                NLS::Base::Profiling::ProfilerCapability_CPUScopes,
                ""
            };
        }

        std::mutex mutex;
        std::vector<RecordedProfilerScope> events;
    };

    struct TypedCounterJob : NLS::Base::Jobs::IJob
    {
        std::atomic<int>* counter = nullptr;
        int amount = 1;

        void Execute()
        {
            counter->fetch_add(amount, std::memory_order_acq_rel);
        }
    };

    struct TypedParallelCounterJob : NLS::Base::Jobs::IJobParallelFor
    {
        std::vector<std::atomic<int>>* visits = nullptr;
        int amount = 1;

        void Execute(const uint32_t index)
        {
            (*visits)[index].fetch_add(amount, std::memory_order_acq_rel);
        }
    };

    struct TypedParallelMutableJob : NLS::Base::Jobs::IJobParallelFor
    {
        std::atomic<int>* concurrentExecutions = nullptr;
        std::atomic<int>* maxConcurrentExecutions = nullptr;
        std::atomic<uintptr_t>* firstExecutingInstance = nullptr;
        std::atomic<uintptr_t>* secondExecutingInstance = nullptr;
        std::atomic<bool>* release = nullptr;

        void Execute(uint32_t)
        {
            const auto instance = reinterpret_cast<uintptr_t>(this);
            uintptr_t expectedFirst = 0u;
            if (!firstExecutingInstance->compare_exchange_strong(
                    expectedFirst,
                    instance,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire) &&
                expectedFirst != instance)
            {
                uintptr_t expectedSecond = 0u;
                (void)secondExecutingInstance->compare_exchange_strong(
                    expectedSecond,
                    instance,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire);
            }

            const int running = concurrentExecutions->fetch_add(1, std::memory_order_acq_rel) + 1;
            int observed = maxConcurrentExecutions->load(std::memory_order_acquire);
            while (running > observed &&
                !maxConcurrentExecutions->compare_exchange_weak(
                    observed,
                    running,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
            }

            while (!release->load(std::memory_order_acquire))
                std::this_thread::yield();

            concurrentExecutions->fetch_sub(1, std::memory_order_acq_rel);
        }
    };
}

TEST_F(JobSystemParallelTests, DifferentJobsConcurrentCompletesAllJobsUnderOneHandle)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterJobData first{&counter};
    CounterJobData second{&counter};
    CounterJobData third{&counter};
    NLS::Base::Jobs::JobScheduleDesc jobs[3];
    jobs[0].function = IncrementCounterJob;
    jobs[0].userData = &first;
    jobs[1].function = IncrementCounterJob;
    jobs[1].userData = &second;
    jobs[2].function = IncrementCounterJob;
    jobs[2].userData = &third;

    auto handle = NLS::Base::Jobs::ScheduleDifferentJobsConcurrent(jobs, 3u);
    NLS::Base::Jobs::Complete(handle);

    EXPECT_EQ(counter.load(std::memory_order_acquire), 3);
    EXPECT_TRUE(NLS::Base::Jobs::HasBeenSynced(handle));
}

TEST_F(JobSystemParallelTests, JobDebugNamesAreOwnedAfterScheduleReturns)
{
    NLS::Base::Profiling::Profiler::ResetForTesting();
    RecordingProfilerDestination destination;
    NLS::Base::Profiling::Profiler::RegisterDestination(destination);
    NLS::Base::Profiling::Profiler::SetEnabled(true);

    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterJobData data{&counter};
    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = IncrementCounterJob;
    desc.userData = &data;
    std::string debugName = "OwnedForegroundDebugName";
    desc.debugName = debugName.c_str();
    auto handle = NLS::Base::Jobs::ScheduleJob(desc);
    ASSERT_NE(handle.id, 0u);
    debugName.assign(debugName.size(), 'x');

    NLS::Base::Jobs::Complete(handle);

    const auto snapshot = NLS::Base::Jobs::CopyJobDiagnosticSnapshot();
    EXPECT_TRUE(std::any_of(
        snapshot.recentJobs.begin(),
        snapshot.recentJobs.end(),
        [](const NLS::Base::Jobs::JobDiagnosticRecord& record)
        {
            return record.debugName == "OwnedForegroundDebugName";
        }));

    {
        std::lock_guard lock(destination.mutex);
        EXPECT_TRUE(std::any_of(
            destination.events.begin(),
            destination.events.end(),
            [](const RecordedProfilerScope& event)
            {
                return event.phase == "begin" && event.name == "OwnedForegroundDebugName";
            }));
    }
    EXPECT_EQ(counter.load(std::memory_order_acquire), 1);
    NLS::Base::Profiling::Profiler::ResetForTesting();
}

TEST_F(JobSystemParallelTests, IJobScheduleCopiesJobDataAndRunsExecute)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    TypedCounterJob job;
    job.counter = &counter;
    job.amount = 3;

    NLS::Base::Jobs::JobScheduleOptions options;
    options.debugName = "TypedIJob";
    auto handle = NLS::Base::Jobs::Schedule(job, options);
    ASSERT_NE(handle.id, 0u);
    job.amount = 100;

    NLS::Base::Jobs::Complete(handle);

    EXPECT_EQ(counter.load(std::memory_order_acquire), 3);
}

TEST_F(JobSystemParallelTests, IJobParallelForScheduleCopiesJobDataAndVisitsEveryIndex)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 2u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::vector<std::atomic<int>> visits(32u);
    TypedParallelCounterJob job;
    job.visits = &visits;
    job.amount = 1;

    NLS::Base::Jobs::JobParallelForScheduleOptions options;
    options.batchSize = 2u;
    options.debugName = "TypedIJobParallelFor";
    auto handle = NLS::Base::Jobs::ScheduleParallelFor(job, static_cast<uint32_t>(visits.size()), options);
    ASSERT_NE(handle.id, 0u);
    job.amount = 100;

    NLS::Base::Jobs::Complete(handle);

    for (const auto& visit : visits)
        EXPECT_EQ(visit.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemParallelTests, IJobParallelForUsesSeparateMutableJobCopyPerShard)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 2u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> concurrentExecutions = 0;
    std::atomic<int> maxConcurrentExecutions = 0;
    std::atomic<uintptr_t> firstExecutingInstance = 0u;
    std::atomic<uintptr_t> secondExecutingInstance = 0u;
    std::atomic<bool> release = false;

    TypedParallelMutableJob job;
    job.concurrentExecutions = &concurrentExecutions;
    job.maxConcurrentExecutions = &maxConcurrentExecutions;
    job.firstExecutingInstance = &firstExecutingInstance;
    job.secondExecutingInstance = &secondExecutingInstance;
    job.release = &release;

    NLS::Base::Jobs::JobParallelForScheduleOptions options;
    options.batchSize = 1u;
    auto handle = NLS::Base::Jobs::ScheduleParallelFor(job, 3u, options);
    ASSERT_NE(handle.id, 0u);

    const auto waitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (maxConcurrentExecutions.load(std::memory_order_acquire) < 2 &&
        std::chrono::steady_clock::now() < waitDeadline)
    {
        std::this_thread::yield();
    }
    if (maxConcurrentExecutions.load(std::memory_order_acquire) < 2)
        release.store(true, std::memory_order_release);
    ASSERT_GE(maxConcurrentExecutions.load(std::memory_order_acquire), 2);

    release.store(true, std::memory_order_release);
    NLS::Base::Jobs::Complete(handle);

    EXPECT_NE(firstExecutingInstance.load(std::memory_order_acquire), 0u);
    EXPECT_NE(secondExecutingInstance.load(std::memory_order_acquire), 0u);
    EXPECT_NE(
        firstExecutingInstance.load(std::memory_order_acquire),
        secondExecutingInstance.load(std::memory_order_acquire));
}

TEST_F(JobSystemParallelTests, DifferentJobsConcurrentRunsPerJobCancelCallbacksWhenCancelledBeforeStart)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS::Base::Jobs::JobScheduleDesc failureDesc;
    failureDesc.function = ThrowingJob;
    auto failedDependency = NLS::Base::Jobs::ScheduleJob(failureDesc);
    ASSERT_NE(failedDependency.id, 0u);
    NLS::Base::Jobs::CompleteNoClear(failedDependency);

    std::atomic<int> cancelCount = 0;
    std::atomic<int> runCount = 0;
    CounterJobData runData{&runCount};
    NLS::Base::Jobs::JobScheduleDesc jobs[2];
    for (auto& job : jobs)
    {
        job.function = IncrementCounterJob;
        job.userData = &runData;
        job.cancelFunction = CountCancel;
        job.cancelUserData = &cancelCount;
    }

    auto handle = NLS::Base::Jobs::ScheduleDifferentJobsConcurrent(jobs, std::size(jobs), failedDependency);
    ASSERT_NE(handle.id, 0u);

    NLS::Base::Jobs::Complete(handle);

    EXPECT_EQ(runCount.load(std::memory_order_acquire), 0);
    EXPECT_EQ(cancelCount.load(std::memory_order_acquire), 2);
}

TEST_F(JobSystemParallelTests, DifferentJobsConcurrentDoesNotRunPerJobCancelCallbacksAfterSuccess)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> destroyed = 0;
    std::atomic<int> cancelCount = 0;
    std::atomic<int> runCount = 0;
    CounterJobData runData{&runCount};
    CountedPayload cancelPayloads[2] = {
        {&destroyed, &cancelCount, false},
        {&destroyed, &cancelCount, false}};

    NLS::Base::Jobs::JobScheduleDesc jobs[2];
    for (size_t index = 0u; index < std::size(jobs); ++index)
    {
        auto& job = jobs[index];
        job.function = IncrementCounterJob;
        job.userData = &runData;
        job.cancelFunction = CountAndMaybeDestroyCancel;
        job.cancelUserData = &cancelPayloads[index];
    }

    auto handle = NLS::Base::Jobs::ScheduleDifferentJobsConcurrent(jobs, std::size(jobs));
    ASSERT_NE(handle.id, 0u);
    NLS::Base::Jobs::Complete(handle);

    EXPECT_EQ(runCount.load(std::memory_order_acquire), 2);
    EXPECT_EQ(cancelCount.load(std::memory_order_acquire), 0);
    EXPECT_EQ(destroyed.load(std::memory_order_acquire), 0);
}

TEST_F(JobSystemParallelTests, DifferentJobsConcurrentCleansOnlyUnstartedSiblingPayloadsAfterStartedFailure)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> destroyed = 0;
    std::atomic<int> cancelCount = 0;
    std::atomic<int> startedCancelCount = 0;
    std::atomic<int> siblingCancelCount = 0;
    CountedPayload cancelPayloads[2] = {
        {&destroyed, &startedCancelCount, false},
        {&destroyed, &siblingCancelCount, false}};

    NLS::Base::Jobs::JobScheduleDesc jobs[2];
    jobs[0].function = ThrowingJob;
    jobs[0].cancelFunction = CountAndMaybeDestroyCancel;
    jobs[0].cancelUserData = &cancelPayloads[0];
    jobs[1].function = IncrementCounterJob;
    jobs[1].cancelFunction = CountAndMaybeDestroyCancel;
    jobs[1].cancelUserData = &cancelPayloads[1];

    auto handle = NLS::Base::Jobs::ScheduleDifferentJobsConcurrent(jobs, std::size(jobs));
    ASSERT_NE(handle.id, 0u);
    NLS::Base::Jobs::Complete(handle);

    EXPECT_EQ(startedCancelCount.load(std::memory_order_acquire), 0);
    EXPECT_EQ(siblingCancelCount.load(std::memory_order_acquire), 1);
    EXPECT_EQ(destroyed.load(std::memory_order_acquire), 0);
}

TEST_F(JobSystemParallelTests, DifferentJobsConcurrentCleansUnstartedSiblingPayloadsAfterStartedFailure)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> destroyed = 0;
    std::atomic<int> cancelCount = 0;
    std::atomic<int> siblingRunCount = 0;
    CounterJobData siblingData{&siblingRunCount};
    auto* siblingPayload = new CountedPayload{&destroyed, &cancelCount, true};

    NLS::Base::Jobs::JobScheduleDesc jobs[2];
    jobs[0].function = ThrowingJob;
    jobs[1].function = IncrementCounterJob;
    jobs[1].userData = &siblingData;
    jobs[1].cancelFunction = CountAndMaybeDestroyCancel;
    jobs[1].cancelUserData = siblingPayload;

    auto handle = NLS::Base::Jobs::ScheduleDifferentJobsConcurrent(jobs, std::size(jobs));
    ASSERT_NE(handle.id, 0u);
    NLS::Base::Jobs::Complete(handle);

    const int siblingRuns = siblingRunCount.load(std::memory_order_acquire);
    const int siblingCancels = cancelCount.load(std::memory_order_acquire);
    EXPECT_LE(siblingRuns, 1);
    EXPECT_LE(siblingCancels, 1);
    EXPECT_EQ(siblingRuns + siblingCancels, 1);
    EXPECT_EQ(destroyed.load(std::memory_order_acquire), siblingCancels);
    if (siblingCancels == 0)
        delete siblingPayload;
}

TEST_F(JobSystemParallelTests, DifferentJobsConcurrentHonorsPerJobNoSyncWaitPolicy)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> targetCounter = 0;
    CounterJobData targetData{&targetCounter};
    NLS::Base::Jobs::JobScheduleDesc targetDesc;
    targetDesc.function = IncrementCounterJob;
    targetDesc.userData = &targetData;
    auto target = NLS::Base::Jobs::ScheduleJob(targetDesc);

    std::atomic<bool> waiterReturned = false;
    struct CompleteOtherData
    {
        NLS::Base::Jobs::JobHandle* handle = nullptr;
        std::atomic<bool>* returned = nullptr;
    } completeData{&target, &waiterReturned};

    auto completeOther = [](void* userData)
    {
        auto* data = static_cast<CompleteOtherData*>(userData);
        NLS::Base::Jobs::CompleteNoClear(*data->handle);
        data->returned->store(true, std::memory_order_release);
    };

    NLS::Base::Jobs::JobScheduleDesc jobs[2];
    jobs[0].function = completeOther;
    jobs[0].userData = &completeData;
    jobs[0].safetyPolicy = NLS::Base::Jobs::JobSafetyPolicy::GuaranteedNoSyncWait;
    std::atomic<int> siblingCounter = 0;
    CounterJobData siblingData{&siblingCounter};
    jobs[1].function = IncrementCounterJob;
    jobs[1].userData = &siblingData;

    auto handle = NLS::Base::Jobs::ScheduleDifferentJobsConcurrent(jobs, std::size(jobs));
    NLS::Base::Jobs::Complete(handle);

    EXPECT_TRUE(waiterReturned.load(std::memory_order_acquire));
    EXPECT_EQ(targetCounter.load(std::memory_order_acquire), 0);
    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(target));
    EXPECT_EQ(siblingCounter.load(std::memory_order_acquire), 1);

    const auto snapshot = NLS::Base::Jobs::CopyJobDiagnosticSnapshot();
    EXPECT_TRUE(std::any_of(
        snapshot.recentViolations.begin(),
        snapshot.recentViolations.end(),
        [](const NLS::Base::Jobs::JobViolationRecord& record)
        {
            return record.kind == NLS::Base::Jobs::JobViolationKind::SyncWaitDisallowed;
        }));
    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
}

TEST_F(JobSystemParallelTests, DifferentJobsConcurrentDiagnosticsReturnToZeroRunningCount)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterJobData first{&counter};
    CounterJobData second{&counter};
    CounterJobData third{&counter};
    NLS::Base::Jobs::JobScheduleDesc jobs[3];
    jobs[0].function = IncrementCounterJob;
    jobs[0].userData = &first;
    jobs[1].function = IncrementCounterJob;
    jobs[1].userData = &second;
    jobs[2].function = IncrementCounterJob;
    jobs[2].userData = &third;

    auto handle = NLS::Base::Jobs::ScheduleDifferentJobsConcurrent(jobs, 3u);
    NLS::Base::Jobs::Complete(handle);

    const auto snapshot = NLS::Base::Jobs::CopyJobDiagnosticSnapshot();
    EXPECT_EQ(counter.load(std::memory_order_acquire), 3);
    EXPECT_EQ(snapshot.runningJobCount, 0u);
    EXPECT_EQ(snapshot.queuedJobCount, 0u);
}

TEST_F(JobSystemParallelTests, ParallelForVisitsEveryIndexExactlyOnce)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::vector<int> visits(64u, 0);
    std::atomic<int> totalVisits = 0;
    ForEachData data{&visits, &totalVisits};

    NLS::Base::Jobs::JobForEachDesc desc;
    desc.function = VisitIndexJob;
    desc.userData = &data;
    desc.iterationCount = static_cast<uint32_t>(visits.size());

    auto handle = NLS::Base::Jobs::ScheduleJobForEach(desc);
    NLS::Base::Jobs::Complete(handle);

    EXPECT_EQ(totalVisits.load(std::memory_order_acquire), static_cast<int>(visits.size()));
    for (const int visit : visits)
        EXPECT_EQ(visit, 1);
}

TEST_F(JobSystemParallelTests, ParallelForMultiWorkerVisitsEveryIndexExactlyOnce)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 4u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::vector<std::atomic<int>> visits(4096u);
    std::atomic<int> totalVisits = 0;
    AtomicForEachData data{&visits, &totalVisits};

    NLS::Base::Jobs::JobForEachDesc desc;
    desc.function = VisitIndexAtomicJob;
    desc.userData = &data;
    desc.iterationCount = static_cast<uint32_t>(visits.size());
    desc.batchSize = 3u;

    auto handle = NLS::Base::Jobs::ScheduleJobForEach(desc);
    NLS::Base::Jobs::Complete(handle);

    EXPECT_EQ(totalVisits.load(std::memory_order_acquire), static_cast<int>(visits.size()));
    for (const auto& visit : visits)
        EXPECT_EQ(visit.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemParallelTests, ParallelForZeroWorkerModeUsesSingleExecutionLane)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::vector<int> visits(8u, 0);
    std::atomic<int> totalVisits = 0;
    ForEachData data{&visits, &totalVisits};

    NLS::Base::Jobs::JobForEachDesc desc;
    desc.function = VisitIndexJob;
    desc.userData = &data;
    desc.iterationCount = static_cast<uint32_t>(visits.size());
    desc.batchSize = 1u;

    auto handle = NLS::Base::Jobs::ScheduleJobForEach(desc);
    ASSERT_NE(handle.id, 0u);

    EXPECT_TRUE(NLS::Base::Jobs::ExecuteOneJobQueueJob());
    EXPECT_FALSE(NLS::Base::Jobs::ExecuteOneJobQueueJob());
    EXPECT_EQ(totalVisits.load(std::memory_order_acquire), static_cast<int>(visits.size()));
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
}

TEST_F(JobSystemParallelTests, ParallelForZeroIterationsCompletesWithoutCallingCallback)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::vector<int> visits;
    std::atomic<int> totalVisits = 0;
    ForEachData data{&visits, &totalVisits};

    NLS::Base::Jobs::JobForEachDesc desc;
    desc.function = VisitIndexJob;
    desc.userData = &data;
    desc.iterationCount = 0u;

    auto handle = NLS::Base::Jobs::ScheduleJobForEach(desc);

    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
    EXPECT_EQ(totalVisits.load(std::memory_order_acquire), 0);
}

TEST_F(JobSystemParallelTests, ParallelForRejectsIterationCountsThatCannotFitNativeRangePlan)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::vector<int> visits;
    std::atomic<int> totalVisits = 0;
    ForEachData data{&visits, &totalVisits};

    NLS::Base::Jobs::JobForEachDesc desc;
    desc.function = VisitIndexJob;
    desc.userData = &data;
    desc.iterationCount = static_cast<uint32_t>(std::numeric_limits<int>::max()) + 1u;

    auto handle = NLS::Base::Jobs::ScheduleJobForEach(desc);

    EXPECT_EQ(handle.id, 0u);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
    EXPECT_EQ(totalVisits.load(std::memory_order_acquire), 0);
}

TEST_F(JobSystemParallelTests, ParallelForAcceptsIntMaxRangePlannerBoundary)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::vector<int> visits;
    std::atomic<int> totalVisits = 0;
    ForEachData data{&visits, &totalVisits};

    NLS::Base::Jobs::JobForEachDesc desc;
    desc.function = VisitIndexJob;
    desc.userData = &data;
    desc.iterationCount = static_cast<uint32_t>(std::numeric_limits<int>::max());
    desc.batchSize = static_cast<uint32_t>(std::numeric_limits<int>::max());

    auto handle = NLS::Base::Jobs::ScheduleJobForEach(desc);
    ASSERT_NE(handle.id, 0u);

    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);

    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
    EXPECT_EQ(totalVisits.load(std::memory_order_acquire), 0);
}

TEST_F(JobSystemParallelTests, WorkStealingRangeHandlesFinalIntMaxBatchWithoutOverflow)
{
    NLS::Base::Jobs::WorkStealingRange range;
    NLS::Base::Jobs::InitializeWorkStealingRange(
        range,
        std::numeric_limits<int>::max(),
        2,
        1);
    const int finalPhase = range.phaseCount - 1;
    const int64_t totalBatches =
        (static_cast<int64_t>(range.totalIterationCount) + range.batchSize - 1) / range.batchSize;
    const int64_t finalPhaseBase = static_cast<int64_t>(finalPhase) * range.batchesPerPhase;
    const int finalPhaseBatchCount = static_cast<int>(totalBatches - finalPhaseBase);
    ASSERT_GT(finalPhaseBatchCount, 0);
    ASSERT_LE(finalPhaseBatchCount, range.batchesPerPhase);

    range.currentPhaseByJob[0] = finalPhase;
    range.startEndByJobAndPhase[static_cast<size_t>(finalPhase)] =
        PackTestWorkStealingSlot(finalPhaseBatchCount - 1, finalPhaseBatchCount);

    int beginIndex = 0;
    int endIndex = 0;

    ASSERT_TRUE(NLS::Base::Jobs::GetWorkStealingRange(range, 0, beginIndex, endIndex));

    EXPECT_EQ(beginIndex, std::numeric_limits<int>::max() - 1);
    EXPECT_EQ(endIndex, std::numeric_limits<int>::max());
}

TEST_F(JobSystemParallelTests, WorkStealingRangeClampsOversizedPublicJobCount)
{
    NLS::Base::Jobs::WorkStealingRange range;
    NLS::Base::Jobs::InitializeWorkStealingRange(
        range,
        1,
        1,
        std::numeric_limits<int>::max());

    EXPECT_EQ(range.jobCount, 1);
    EXPECT_EQ(range.startEndByJobAndPhase.size(), 1u);
    EXPECT_EQ(range.currentPhaseByJob.size(), 1u);
}

TEST_F(JobSystemParallelTests, ParallelForRejectsBatchSizeThatCannotFitNativeRangePlan)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::vector<int> visits(1u, 0);
    std::atomic<int> totalVisits = 0;
    ForEachData data{&visits, &totalVisits};

    NLS::Base::Jobs::JobForEachDesc desc;
    desc.function = VisitIndexJob;
    desc.userData = &data;
    desc.iterationCount = 1u;
    desc.batchSize = static_cast<uint32_t>(std::numeric_limits<int>::max()) + 1u;

    auto handle = NLS::Base::Jobs::ScheduleJobForEach(desc);

    EXPECT_EQ(handle.id, 0u);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
    EXPECT_EQ(totalVisits.load(std::memory_order_acquire), 0);
}

TEST_F(JobSystemParallelTests, ParallelForImmediateShutdownCancelsQueuedPayloadsWithoutDeadlock)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::vector<int> visits(8u, 0);
    std::atomic<int> totalVisits = 0;
    ForEachData data{&visits, &totalVisits};

    NLS::Base::Jobs::JobForEachDesc desc;
    desc.function = VisitIndexJob;
    desc.userData = &data;
    desc.iterationCount = static_cast<uint32_t>(visits.size());

    auto handle = NLS::Base::Jobs::ScheduleJobForEach(desc);
    ASSERT_NE(handle.id, 0u);

    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
}

TEST_F(JobSystemParallelTests, ImmediateShutdownKeepsParallelForPayloadAliveUntilRunningIterationReturns)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 1u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> started = 0;
    std::atomic<bool> release = false;
    BlockingForEachData data{&started, &release};

    NLS::Base::Jobs::JobForEachDesc desc;
    desc.function = BlockingVisitIndex;
    desc.userData = &data;
    desc.iterationCount = 32u;

    auto handle = NLS::Base::Jobs::ScheduleJobForEach(desc);
    ASSERT_NE(handle.id, 0u);

    for (int attempt = 0; attempt < 200 && started.load(std::memory_order_acquire) == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_GT(started.load(std::memory_order_acquire), 0);

    std::thread shutdownThread(
        []
        {
            NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    release.store(true, std::memory_order_release);
    shutdownThread.join();

    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
    EXPECT_GT(started.load(std::memory_order_acquire), 0);
}

TEST_F(JobSystemParallelTests, ImmediateShutdownCancelsQueuedSiblingJobsWhenGroupIsRunning)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> started = 0;
    std::atomic<int> finished = 0;
    std::atomic<bool> release = false;
    BlockingCounterJobData blockingData{&started, &finished, &release};
    std::atomic<int> siblingCounter = 0;
    std::atomic<int> siblingCancelCount = 0;
    CounterJobData siblingData{&siblingCounter};

    NLS::Base::Jobs::JobScheduleDesc jobs[2];
    jobs[0].function = BlockingCounterJob;
    jobs[0].userData = &blockingData;
    jobs[1].function = IncrementCounterJob;
    jobs[1].userData = &siblingData;
    jobs[1].cancelFunction = CountCancel;
    jobs[1].cancelUserData = &siblingCancelCount;

    auto handle = NLS::Base::Jobs::ScheduleDifferentJobsConcurrent(jobs, std::size(jobs));
    ASSERT_NE(handle.id, 0u);

    for (int attempt = 0; attempt < 200 && started.load(std::memory_order_acquire) == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_GT(started.load(std::memory_order_acquire), 0);

    std::thread shutdownThread(
        []
        {
            NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(handle));

    release.store(true, std::memory_order_release);
    shutdownThread.join();

    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
    EXPECT_EQ(finished.load(std::memory_order_acquire), 1);
    EXPECT_EQ(siblingCounter.load(std::memory_order_acquire), 0);
    EXPECT_EQ(siblingCancelCount.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemParallelTests, CombineRunsOnceAfterAllParallelForIterations)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::vector<int> visits(17u, 0);
    std::atomic<int> combineCount = 0;
    std::atomic<int> totalAtCombine = 0;
    CombineData data{&visits, &combineCount, &totalAtCombine};

    NLS::Base::Jobs::JobForEachDesc desc;
    desc.function = VisitIndexForCombineJob;
    desc.userData = &data;
    desc.iterationCount = static_cast<uint32_t>(visits.size());
    desc.combineFunction = CombineJob;

    auto handle = NLS::Base::Jobs::ScheduleJobForEach(desc);
    NLS::Base::Jobs::Complete(handle);

    EXPECT_EQ(combineCount.load(std::memory_order_acquire), 1);
    EXPECT_EQ(totalAtCombine.load(std::memory_order_acquire), static_cast<int>(visits.size()));
}

TEST_F(JobSystemParallelTests, ImmediateShutdownDuringCombineDoesNotRunCancelCleanup)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> combineStarted = 0;
    std::atomic<bool> releaseCombine = false;
    std::atomic<int> combineReturned = 0;
    BlockingCombineData data{&combineStarted, &releaseCombine, &combineReturned};

    NLS::Base::Jobs::JobForEachDesc desc;
    desc.function = NoOpForEach;
    desc.userData = &data;
    desc.iterationCount = 4u;
    desc.combineFunction = BlockingCombine;

    auto handle = NLS::Base::Jobs::ScheduleJobForEach(desc);
    ASSERT_NE(handle.id, 0u);

    std::atomic<bool> shutdownReturned = false;
    std::thread waiter(
        [handle]
        {
            NLS::Base::Jobs::CompleteNoClear(handle);
        });

    for (int attempt = 0; attempt < 200 && combineStarted.load(std::memory_order_acquire) == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(combineStarted.load(std::memory_order_acquire), 1);

    std::thread shutdownThread(
        [&shutdownReturned]
        {
            NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
            shutdownReturned.store(true, std::memory_order_release);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(shutdownReturned.load(std::memory_order_acquire));
    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(handle));
    EXPECT_EQ(combineReturned.load(std::memory_order_acquire), 0);

    releaseCombine.store(true, std::memory_order_release);
    waiter.join();
    shutdownThread.join();

    EXPECT_EQ(combineReturned.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
}

TEST_F(JobSystemParallelTests, ImmediateShutdownDoesNotCompleteWhileCombineIsRunning)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> combineStarted = 0;
    std::atomic<bool> releaseCombine = false;
    std::atomic<int> combineReturned = 0;
    BlockingCombineData data{&combineStarted, &releaseCombine, &combineReturned};

    NLS::Base::Jobs::JobForEachDesc desc;
    desc.function = NoOpForEach;
    desc.userData = &data;
    desc.iterationCount = 1u;
    desc.batchSize = 1u;
    desc.combineFunction = BlockingCombine;
    desc.debugName = "BlockingCombineShutdown";
    auto handle = NLS::Base::Jobs::ScheduleJobForEach(desc);
    ASSERT_NE(handle.id, 0u);

    while (combineStarted.load(std::memory_order_acquire) == 0)
        std::this_thread::yield();

    std::thread shutdownThread(
        []
        {
            NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
        });

    for (int attempt = 0; attempt < 200 && !NLS::Base::Jobs::IsCompleted(handle); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(handle));
    const auto snapshot = NLS::Base::Jobs::CopyJobDiagnosticSnapshot();
    EXPECT_FALSE(std::any_of(
        snapshot.recentJobs.begin(),
        snapshot.recentJobs.end(),
        [](const NLS::Base::Jobs::JobDiagnosticRecord& record)
        {
            return record.debugName == "BlockingCombineShutdown" &&
                record.state == NLS::Base::Jobs::JobLifecycleState::Cancelled;
        }));
    releaseCombine.store(true, std::memory_order_release);
    shutdownThread.join();

    EXPECT_EQ(combineReturned.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
}

TEST_F(JobSystemParallelTests, DrainShutdownDoesNotClearFailedGroupWhileHelperJobIsRunning)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> started = 0;
    std::atomic<int> blockerFinished = 0;
    std::atomic<bool> release = false;
    ThrowingAndBlockingForEachData data{&started, &blockerFinished, &release};

    NLS::Base::Jobs::JobScheduleDesc jobs[2];
    jobs[0].function = ThrowingDrainJob;
    jobs[0].userData = &data;
    jobs[1].function = BlockingDrainJob;
    jobs[1].userData = &data;

    auto handle = NLS::Base::Jobs::ScheduleDifferentJobsConcurrent(jobs, std::size(jobs));
    ASSERT_NE(handle.id, 0u);

    std::thread firstWaiter(
        [handle]
        {
            NLS::Base::Jobs::CompleteNoClear(handle);
        });
    std::thread secondWaiter(
        [handle]
        {
            NLS::Base::Jobs::CompleteNoClear(handle);
        });

    for (int attempt = 0; attempt < 200 && started.load(std::memory_order_acquire) < 2; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (started.load(std::memory_order_acquire) < 2)
    {
        release.store(true, std::memory_order_release);
        firstWaiter.join();
        secondWaiter.join();
        FAIL() << "Expected both helper-run sibling jobs to start.";
    }

    auto hasFailureDiagnostic = [handle]
    {
        const auto snapshot = NLS::Base::Jobs::CopyJobDiagnosticSnapshot();
        return std::any_of(
            snapshot.recentJobs.begin(),
            snapshot.recentJobs.end(),
            [handle](const NLS::Base::Jobs::JobDiagnosticRecord& record)
            {
                return record.id == handle.id &&
                    record.state == NLS::Base::Jobs::JobLifecycleState::Failed;
            });
    };

    for (int attempt = 0; attempt < 200 && !hasFailureDiagnostic(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!hasFailureDiagnostic())
    {
        release.store(true, std::memory_order_release);
        firstWaiter.join();
        secondWaiter.join();
        FAIL() << "Expected the throwing sibling job to record a failed group before drain shutdown.";
    }

    std::atomic<bool> shutdownReturned = false;
    std::thread shutdownThread(
        [&shutdownReturned]
        {
            NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::DrainAcceptedWork);
            shutdownReturned.store(true, std::memory_order_release);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(shutdownReturned.load(std::memory_order_acquire));
    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(handle));
    EXPECT_EQ(blockerFinished.load(std::memory_order_acquire), 0);

    release.store(true, std::memory_order_release);
    firstWaiter.join();
    secondWaiter.join();
    shutdownThread.join();

    EXPECT_EQ(blockerFinished.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
}

TEST_F(JobSystemParallelTests, ThrowingParallelForShardKeepsPayloadAliveUntilRunningShardsReturn)
{
    constexpr auto kDebugCiShardStartTimeout = std::chrono::seconds(5);
    constexpr auto kFailureDiagnosticTimeout = std::chrono::seconds(5);
    auto waitUntil = [](auto&& predicate, const std::chrono::steady_clock::duration timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!predicate())
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::yield();
        }

        return true;
    };

    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 2u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> started = 0;
    std::atomic<int> afterThrowerReturned = 0;
    std::atomic<bool> allowThrow = false;
    std::atomic<bool> release = false;
    ThrowingForEachData data{&started, &afterThrowerReturned, &allowThrow, &release};

    NLS::Base::Jobs::JobForEachDesc desc;
    desc.function = ThrowingOrBlockingVisitIndex;
    desc.userData = &data;
    desc.iterationCount = 32u;
    desc.debugName = "ThrowingParallelForShardLifetime";

    auto handle = NLS::Base::Jobs::ScheduleJobForEach(desc);
    ASSERT_NE(handle.id, 0u);

    auto hasTwoStartedShards = [&started]
    {
        return started.load(std::memory_order_acquire) >= 2;
    };
    if (!waitUntil(hasTwoStartedShards, kDebugCiShardStartTimeout))
    {
        allowThrow.store(true, std::memory_order_release);
        release.store(true, std::memory_order_release);
        NLS::Base::Jobs::CompleteNoClear(handle);
        FAIL() << "Expected the throwing shard and at least one blocking shard to start; started="
            << started.load(std::memory_order_acquire);
    }

    allowThrow.store(true, std::memory_order_release);

    auto hasFailureDiagnostic = [handle]
    {
        const auto snapshot = NLS::Base::Jobs::CopyJobDiagnosticSnapshot();
        return std::any_of(
            snapshot.recentJobs.begin(),
            snapshot.recentJobs.end(),
            [handle](const NLS::Base::Jobs::JobDiagnosticRecord& record)
            {
                return record.id == handle.id &&
                    record.generation == handle.generation &&
                    record.state == NLS::Base::Jobs::JobLifecycleState::Failed;
            });
    };

    if (!waitUntil(hasFailureDiagnostic, kFailureDiagnosticTimeout))
    {
        release.store(true, std::memory_order_release);
        NLS::Base::Jobs::CompleteNoClear(handle);
        FAIL() << "Expected the throwing shard to record a failed group before releasing blockers; started="
            << started.load(std::memory_order_acquire) << " failedDiagnostic=0";
    }

    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(handle));
    EXPECT_EQ(afterThrowerReturned.load(std::memory_order_acquire), 0);

    std::thread waiter(
        [handle]
        {
            NLS::Base::Jobs::CompleteNoClear(handle);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(afterThrowerReturned.load(std::memory_order_acquire), 0);

    release.store(true, std::memory_order_release);
    waiter.join();

    EXPECT_GT(afterThrowerReturned.load(std::memory_order_acquire), 0);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
}

TEST_F(JobSystemParallelTests, BlockRangesCoverArrayWithBoundedJobCount)
{
    NLS::Base::Jobs::JobBlockRange ranges[8];
    const int count = NLS::Base::Jobs::ConfigureBlockRangesWithMinIndicesPerJob(
        ranges,
        25,
        4,
        8);

    ASSERT_GT(count, 0);
    ASSERT_LE(count, 7);

    size_t expectedStart = 0u;
    size_t covered = 0u;
    for (int index = 0; index < count; ++index)
    {
        EXPECT_EQ(ranges[index].startIndex, expectedStart);
        EXPECT_EQ(ranges[index].rangesTotal, static_cast<size_t>(count));
        expectedStart += ranges[index].rangeSize;
        covered += ranges[index].rangeSize;
    }
    EXPECT_EQ(covered, 25u);
}

TEST_F(JobSystemParallelTests, WorkStealingRangeClaimsEveryIndexOnceForUnevenWorkload)
{
    NLS::Base::Jobs::WorkStealingRange range;
    NLS::Base::Jobs::InitializeWorkStealingRange(range, 37, 3, 4);

    std::vector<int> visits(37u, 0);
    std::array<int, 4> claimCounts{};

    for (int jobIndex = 0; jobIndex < 4; ++jobIndex)
    {
        int beginIndex = 0;
        int endIndex = 0;
        while (NLS::Base::Jobs::GetWorkStealingRange(range, jobIndex, beginIndex, endIndex))
        {
            ASSERT_GE(beginIndex, 0);
            ASSERT_LE(endIndex, static_cast<int>(visits.size()));
            ASSERT_LT(beginIndex, endIndex);

            ++claimCounts[static_cast<size_t>(jobIndex)];
            for (int index = beginIndex; index < endIndex; ++index)
                ++visits[static_cast<size_t>(index)];
        }
    }

    for (const int visit : visits)
        EXPECT_EQ(visit, 1);

    int totalClaims = 0;
    for (const int claims : claimCounts)
        totalClaims += claims;
    EXPECT_EQ(totalClaims, 13);
}

TEST_F(JobSystemParallelTests, BatchDispatcherDoesNotExposeJobsUntilKick)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterJobData data{&counter};
    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = IncrementCounterJob;
    desc.userData = &data;

    NLS::Base::Jobs::JobBatchDispatcher dispatcher;
    auto handle = dispatcher.AddJob(desc);

    EXPECT_FALSE(dispatcher.Empty());
    EXPECT_FALSE(NLS::Base::Jobs::ExecuteOneJobQueueJob());
    EXPECT_EQ(counter.load(std::memory_order_acquire), 0);

    dispatcher.Kick();
    NLS::Base::Jobs::Complete(handle);

    EXPECT_TRUE(dispatcher.Empty());
    EXPECT_EQ(counter.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemParallelTests, BatchDispatcherForEachDoesNotExposeIterationsUntilKick)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::vector<int> visits(9u, 0);
    std::atomic<int> totalVisits = 0;
    ForEachData data{&visits, &totalVisits};

    NLS::Base::Jobs::JobForEachDesc desc;
    desc.function = VisitIndexJob;
    desc.userData = &data;
    desc.iterationCount = static_cast<uint32_t>(visits.size());

    NLS::Base::Jobs::JobBatchDispatcher dispatcher;
    auto handle = dispatcher.AddForEach(desc);

    EXPECT_NE(handle.id, 0u);
    EXPECT_FALSE(dispatcher.Empty());
    EXPECT_FALSE(NLS::Base::Jobs::ExecuteOneJobQueueJob());
    EXPECT_EQ(totalVisits.load(std::memory_order_acquire), 0);

    dispatcher.Kick();
    NLS::Base::Jobs::Complete(handle);

    EXPECT_TRUE(dispatcher.Empty());
    EXPECT_EQ(totalVisits.load(std::memory_order_acquire), static_cast<int>(visits.size()));
    for (const int visit : visits)
        EXPECT_EQ(visit, 1);
}

TEST_F(JobSystemParallelTests, CompletingUnkickedBatchDispatcherJobKicksPendingWork)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterJobData data{&counter};
    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = IncrementCounterJob;
    desc.userData = &data;

    NLS::Base::Jobs::JobBatchDispatcher dispatcher;
    auto handle = dispatcher.AddJob(desc);
    ASSERT_NE(handle.id, 0u);

    NLS::Base::Jobs::Complete(handle);

    EXPECT_EQ(counter.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
}

TEST_F(JobSystemParallelTests, IsCompletedOnUnkickedBatchDispatcherJobKicksPendingWork)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterJobData data{&counter};
    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = IncrementCounterJob;
    desc.userData = &data;

    NLS::Base::Jobs::JobBatchDispatcher dispatcher;
    auto handle = dispatcher.AddJob(desc);
    ASSERT_NE(handle.id, 0u);

    EXPECT_FALSE(NLS::Base::Jobs::ExecuteOneJobQueueJob());
    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(handle));

    EXPECT_TRUE(NLS::Base::Jobs::ExecuteOneJobQueueJob());
    EXPECT_EQ(counter.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
}

TEST_F(JobSystemParallelTests, ShutdownDrainRunsUnkickedBatchDispatcherJobsWithoutExternalKick)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterJobData data{&counter};
    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = IncrementCounterJob;
    desc.userData = &data;

    NLS::Base::Jobs::JobBatchDispatcher dispatcher;
    auto handle = dispatcher.AddJob(desc);
    ASSERT_NE(handle.id, 0u);

    std::atomic<bool> shutdownFinished = false;
    std::thread shutdownThread(
        [&shutdownFinished]
        {
            NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::DrainAcceptedWork);
            shutdownFinished.store(true, std::memory_order_release);
        });

    for (int attempt = 0; attempt < 200 && !shutdownFinished.load(std::memory_order_acquire); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    const bool finishedWithoutExternalKick = shutdownFinished.load(std::memory_order_acquire);
    if (!finishedWithoutExternalKick)
        dispatcher.Kick();

    shutdownThread.join();

    EXPECT_TRUE(finishedWithoutExternalKick);
    EXPECT_EQ(counter.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemParallelTests, BatchDispatcherDestructorKicksPendingJobs)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterJobData data{&counter};
    NLS::Base::Jobs::JobHandle handle;
    {
        NLS::Base::Jobs::JobBatchDispatcher dispatcher;
        NLS::Base::Jobs::JobScheduleDesc desc;
        desc.function = IncrementCounterJob;
        desc.userData = &data;
        handle = dispatcher.AddJob(desc);
        ASSERT_NE(handle.id, 0u);
        EXPECT_FALSE(NLS::Base::Jobs::ExecuteOneJobQueueJob());
    }

    EXPECT_TRUE(NLS::Base::Jobs::ExecuteOneJobQueueJob());
    EXPECT_EQ(counter.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
}

TEST_F(JobSystemParallelTests, BatchDispatcherAutoKicksAtConfiguredBatchSize)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterJobData first{&counter};
    CounterJobData second{&counter};

    NLS::Base::Jobs::JobBatchDispatcher dispatcher(2);
    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = IncrementCounterJob;
    desc.userData = &first;
    auto firstHandle = dispatcher.AddJob(desc);
    ASSERT_NE(firstHandle.id, 0u);
    EXPECT_FALSE(NLS::Base::Jobs::ExecuteOneJobQueueJob());

    desc.userData = &second;
    auto secondHandle = dispatcher.AddJob(desc);
    ASSERT_NE(secondHandle.id, 0u);

    EXPECT_TRUE(dispatcher.Empty());
    EXPECT_TRUE(NLS::Base::Jobs::ExecuteOneJobQueueJob());
    EXPECT_TRUE(NLS::Base::Jobs::ExecuteOneJobQueueJob());
    EXPECT_EQ(counter.load(std::memory_order_acquire), 2);
}

TEST_F(JobSystemParallelTests, BatchDispatcherByWorkerCountResolvesAfterInitialization)
{
    NLS::Base::Jobs::JobBatchDispatcher dispatcher(
        NLS::Base::Jobs::JobBatchDispatcher::kBatchKickByWorkerCount);

    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 2u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterJobData first{&counter};
    CounterJobData second{&counter};

    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = IncrementCounterJob;
    desc.userData = &first;
    auto firstHandle = dispatcher.AddJob(desc);
    ASSERT_NE(firstHandle.id, 0u);
    EXPECT_FALSE(dispatcher.Empty());
    EXPECT_FALSE(NLS::Base::Jobs::ExecuteOneJobQueueJob());

    desc.userData = &second;
    auto secondHandle = dispatcher.AddJob(desc);
    ASSERT_NE(secondHandle.id, 0u);

    EXPECT_TRUE(dispatcher.Empty());
    NLS::Base::Jobs::Complete(firstHandle);
    NLS::Base::Jobs::Complete(secondHandle);
    EXPECT_EQ(counter.load(std::memory_order_acquire), 2);
}

TEST_F(JobSystemParallelTests, BatchDispatcherForEachAutoKicksByIterationWorkload)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::vector<int> visits(8u, 0);
    std::atomic<int> totalVisits = 0;
    ForEachData data{&visits, &totalVisits};
    NLS::Base::Jobs::JobForEachDesc desc;
    desc.function = VisitIndexJob;
    desc.userData = &data;
    desc.iterationCount = static_cast<uint32_t>(visits.size());

    NLS::Base::Jobs::JobBatchDispatcher dispatcher(4);
    auto handle = dispatcher.AddForEach(desc);
    ASSERT_NE(handle.id, 0u);

    EXPECT_TRUE(dispatcher.Empty());
    NLS::Base::Jobs::Complete(handle);
    EXPECT_EQ(totalVisits.load(std::memory_order_acquire), static_cast<int>(visits.size()));
}

TEST_F(JobSystemParallelTests, BatchDispatcherPendingJobPreservesCancelCleanup)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterJobData data{&counter};
    std::atomic<int> cancelCount = 0;

    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = IncrementCounterJob;
    desc.userData = &data;
    desc.cancelFunction = [](void* userData)
    {
        auto* count = static_cast<std::atomic<int>*>(userData);
        count->fetch_add(1, std::memory_order_acq_rel);
    };
    desc.cancelUserData = &cancelCount;

    NLS::Base::Jobs::JobBatchDispatcher dispatcher;
    const auto handle = dispatcher.AddJob(desc);
    ASSERT_NE(handle.id, 0u);

    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);

    EXPECT_EQ(counter.load(std::memory_order_acquire), 0);
    EXPECT_EQ(cancelCount.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
}

TEST_F(JobSystemParallelTests, MultiDependencyJobCompletesAfterAllDependencies)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterJobData first{&counter};
    CounterJobData second{&counter};

    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = IncrementCounterJob;
    desc.userData = &first;
    const auto firstHandle = NLS::Base::Jobs::ScheduleJob(desc);
    desc.userData = &second;
    const auto secondHandle = NLS::Base::Jobs::ScheduleJob(desc);

    const NLS::Base::Jobs::JobHandle dependencies[] = {firstHandle, secondHandle};
    NLS::Base::Jobs::JobBatchDispatcher dispatcher;
    auto fanIn = NLS::Base::Jobs::ScheduleMultiDependencyJob(
        dispatcher,
        dependencies,
        std::size(dependencies));
    ASSERT_NE(fanIn.id, 0u);

    NLS::Base::Jobs::Complete(fanIn);

    EXPECT_EQ(counter.load(std::memory_order_acquire), 2);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(firstHandle));
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(secondHandle));
}

TEST_F(JobSystemParallelTests, MultiDependencyJobWithRepeatedPendingDependencyKicksBatch)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterJobData data{&counter};
    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = IncrementCounterJob;
    desc.userData = &data;

    NLS::Base::Jobs::JobBatchDispatcher dispatcher;
    const auto handle = dispatcher.AddJob(desc);
    ASSERT_NE(handle.id, 0u);
    EXPECT_FALSE(NLS::Base::Jobs::ExecuteOneJobQueueJob());

    const NLS::Base::Jobs::JobHandle dependencies[] = {handle, handle};
    const auto fanIn = NLS::Base::Jobs::ScheduleMultiDependencyJob(
        dispatcher,
        dependencies,
        std::size(dependencies));

    EXPECT_EQ(fanIn, handle);
    EXPECT_TRUE(dispatcher.Empty());
    EXPECT_TRUE(NLS::Base::Jobs::ExecuteOneJobQueueJob());
    EXPECT_EQ(counter.load(std::memory_order_acquire), 1);

    NLS::Base::Jobs::CompleteNoClear(handle);
}

TEST_F(JobSystemParallelTests, MultiDependencyJobFailsWhenAnyDependencyFails)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterJobData data{&counter};
    NLS::Base::Jobs::JobScheduleDesc successDesc;
    successDesc.function = IncrementCounterJob;
    successDesc.userData = &data;
    const auto success = NLS::Base::Jobs::ScheduleJob(successDesc);

    NLS::Base::Jobs::JobScheduleDesc failureDesc;
    failureDesc.function = ThrowingJob;
    const auto failure = NLS::Base::Jobs::ScheduleJob(failureDesc);

    const NLS::Base::Jobs::JobHandle dependencies[] = {success, failure};
    NLS::Base::Jobs::JobBatchDispatcher dispatcher;
    auto fanIn = NLS::Base::Jobs::ScheduleMultiDependencyJob(
        dispatcher,
        dependencies,
        std::size(dependencies));
    ASSERT_NE(fanIn.id, 0u);

    std::atomic<int> dependentCounter = 0;
    CounterJobData dependentData{&dependentCounter};
    NLS::Base::Jobs::JobScheduleDesc dependentDesc;
    dependentDesc.function = IncrementCounterJob;
    dependentDesc.userData = &dependentData;
    dependentDesc.dependency = fanIn;
    auto dependent = NLS::Base::Jobs::ScheduleJob(dependentDesc);

    NLS::Base::Jobs::Complete(dependent);

    EXPECT_EQ(counter.load(std::memory_order_acquire), 1);
    EXPECT_EQ(dependentCounter.load(std::memory_order_acquire), 0);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(fanIn));
}

TEST_F(JobSystemParallelTests, DependencyOnlyFanInCancelCleanupRunsOutsideQueueLock)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> successCounter = 0;
    CounterJobData successData{&successCounter};
    NLS::Base::Jobs::JobScheduleDesc successDesc;
    successDesc.function = IncrementCounterJob;
    successDesc.userData = &successData;
    const auto success = NLS::Base::Jobs::ScheduleJob(successDesc);

    NLS::Base::Jobs::JobScheduleDesc failureDesc;
    failureDesc.function = ThrowingJob;
    const auto failure = NLS::Base::Jobs::ScheduleJob(failureDesc);
    NLS::Base::Jobs::CompleteNoClear(failure);

    std::atomic<int> reentrantCounter = 0;
    std::atomic<bool> cancelReturned = false;
    ReentrantCancelData cancelData{&reentrantCounter, &cancelReturned};
    NLS::Base::Jobs::JobBatchDispatcher dispatcher;
    const NLS::Base::Jobs::JobHandle successDependencies[] = {success};
    const auto successFanIn = NLS::Base::Jobs::ScheduleMultiDependencyJob(
        dispatcher,
        successDependencies,
        std::size(successDependencies));
    ASSERT_NE(successFanIn.id, 0u);

    const NLS::Base::Jobs::JobHandle mixedDependencies[] = {successFanIn, failure};
    auto dependent = NLS::Base::Jobs::Internal::ScheduleMultiDependencyFence(
        mixedDependencies,
        std::size(mixedDependencies));
    ASSERT_NE(dependent.id, 0u);
    NLS::Base::Jobs::JobScheduleDesc cleanupDesc;
    cleanupDesc.function = IncrementCounterJob;
    CounterJobData cleanupJobData{&reentrantCounter};
    cleanupDesc.userData = &cleanupJobData;
    cleanupDesc.cancelFunction = ReentrantScheduleFromCancel;
    cleanupDesc.cancelUserData = &cancelData;
    auto cleanupDependent = NLS::Base::Jobs::ScheduleJobDepends(cleanupDesc, dependent);
    ASSERT_NE(cleanupDependent.id, 0u);

    std::atomic<bool> completeReturned = false;
    std::thread waiter(
        [&]
        {
            NLS::Base::Jobs::CompleteNoClear(cleanupDependent);
            completeReturned.store(true, std::memory_order_release);
        });

    for (int attempt = 0; attempt < 200 && !completeReturned.load(std::memory_order_acquire); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    const bool returnedBeforeShutdown = completeReturned.load(std::memory_order_acquire);
    if (!returnedBeforeShutdown)
        NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);

    waiter.join();
    if (returnedBeforeShutdown)
        NLS::Base::Jobs::CompleteNoClear(success);

    EXPECT_TRUE(returnedBeforeShutdown);
    EXPECT_TRUE(cancelReturned.load(std::memory_order_acquire));
    EXPECT_EQ(successCounter.load(std::memory_order_acquire), 1);
    EXPECT_EQ(reentrantCounter.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(dependent));
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(cleanupDependent));
}

TEST_F(JobSystemParallelTests, MultiDependencyJobRejectsUnknownDependency)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    const NLS::Base::Jobs::JobHandle dependencies[] = {{0x1234u, 77u}};
    NLS::Base::Jobs::JobBatchDispatcher dispatcher;
    auto fanIn = NLS::Base::Jobs::ScheduleMultiDependencyJob(
        dispatcher,
        dependencies,
        std::size(dependencies));

    EXPECT_EQ(fanIn.id, 0u);
}

TEST_F(JobSystemParallelTests, PendingForEachWithAlreadyFailedDependencyReleasesPayloadOnCancel)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS::Base::Jobs::JobScheduleDesc failureDesc;
    failureDesc.function = ThrowingJob;
    auto failure = NLS::Base::Jobs::ScheduleJob(failureDesc);
    ASSERT_NE(failure.id, 0u);
    NLS::Base::Jobs::CompleteNoClear(failure);

    std::vector<int> visits(4u, 0);
    std::atomic<int> totalVisits = 0;
    ForEachData data{&visits, &totalVisits};
    NLS::Base::Jobs::JobForEachDesc desc;
    desc.function = VisitIndexJob;
    desc.userData = &data;
    desc.iterationCount = static_cast<uint32_t>(visits.size());
    desc.dependency = failure;

    NLS::Base::Jobs::JobBatchDispatcher dispatcher;
    auto handle = dispatcher.AddForEach(desc);
    ASSERT_NE(handle.id, 0u);

    NLS::Base::Jobs::Complete(handle);

    EXPECT_EQ(totalVisits.load(std::memory_order_acquire), 0);
}
