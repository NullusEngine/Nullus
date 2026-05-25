#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "Jobs/BackgroundJobQueue.h"
#include "Jobs/JobBatchDispatcher.h"
#include "Jobs/JobBindings.h"
#include "Jobs/JobDiagnostics.h"
#include "Jobs/JobRange.h"
#include "Jobs/JobSafety.h"
#include "Jobs/JobSystem.h"

namespace
{
    class JobSystemSchedulerTests : public ::testing::Test
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

    struct OrderJobData
    {
        std::mutex* mutex = nullptr;
        std::vector<int>* order = nullptr;
        int value = 0;
    };

    void PushOrderJob(void* userData)
    {
        auto* data = static_cast<OrderJobData*>(userData);
        std::lock_guard lock(*data->mutex);
        data->order->push_back(data->value);
    }

    struct FlagJobData
    {
        std::atomic<bool>* source = nullptr;
        std::atomic<bool>* observed = nullptr;
    };

    void ObserveFlagJob(void* userData)
    {
        auto* data = static_cast<FlagJobData*>(userData);
        data->observed->store(data->source->load(std::memory_order_acquire), std::memory_order_release);
    }

    void SetFlagJob(void* userData)
    {
        auto* flag = static_cast<std::atomic<bool>*>(userData);
        flag->store(true, std::memory_order_release);
    }

    void ThrowingJob(void*)
    {
        throw std::runtime_error("expected job failure");
    }

    void CountCancel(void* userData)
    {
        auto* counter = static_cast<std::atomic<int>*>(userData);
        counter->fetch_add(1, std::memory_order_acq_rel);
    }

    struct BlockingCounterJobData
    {
        std::atomic<int>* started = nullptr;
        std::atomic<int>* finished = nullptr;
        std::atomic<bool>* release = nullptr;
    };

    void BlockingCounterJob(void* userData)
    {
        auto* data = static_cast<BlockingCounterJobData*>(userData);
        data->started->fetch_add(1, std::memory_order_acq_rel);
        while (!data->release->load(std::memory_order_acquire))
            std::this_thread::yield();
        data->finished->fetch_add(1, std::memory_order_acq_rel);
    }

    void BlockingCancel(void* userData)
    {
        auto* data = static_cast<BlockingCounterJobData*>(userData);
        data->started->fetch_add(1, std::memory_order_acq_rel);
        while (!data->release->load(std::memory_order_acquire))
            std::this_thread::yield();
        data->finished->fetch_add(1, std::memory_order_acq_rel);
    }

    struct ShutdownFromJobData
    {
        std::atomic<bool>* returned = nullptr;
    };

    void ShutdownFromJob(void* userData)
    {
        NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
        auto* data = static_cast<ShutdownFromJobData*>(userData);
        data->returned->store(true, std::memory_order_release);
    }
}

TEST_F(JobSystemSchedulerTests, LifecycleInitializesWithRequestedWorkerCount)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 2u;

    EXPECT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));
    EXPECT_TRUE(NLS::Base::Jobs::IsJobSystemInitialized());
    EXPECT_EQ(NLS::Base::Jobs::GetJobWorkerCount(), 2u);
}

TEST_F(JobSystemSchedulerTests, TryInitializeReportsOwnershipOnlyForNewRuntime)
{
    NLS::Base::Jobs::JobSystemConfig ownerConfig;
    ownerConfig.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::TryInitializeJobSystem(ownerConfig));
    ASSERT_TRUE(NLS::Base::Jobs::IsJobSystemInitialized());
    ASSERT_EQ(NLS::Base::Jobs::GetJobWorkerCount(), 0u);

    NLS::Base::Jobs::JobSystemConfig nonOwnerConfig;
    nonOwnerConfig.workerCount = 2u;
    EXPECT_FALSE(NLS::Base::Jobs::TryInitializeJobSystem(nonOwnerConfig));
    EXPECT_TRUE(NLS::Base::Jobs::IsJobSystemInitialized());
    EXPECT_EQ(NLS::Base::Jobs::GetJobWorkerCount(), 0u);
}

TEST_F(JobSystemSchedulerTests, LifecycleClampsExcessiveWorkerCount)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 65u;

    EXPECT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));
    EXPECT_TRUE(NLS::Base::Jobs::IsJobSystemInitialized());
    EXPECT_EQ(NLS::Base::Jobs::GetJobWorkerCount(), 64u);
}

TEST_F(JobSystemSchedulerTests, LifecycleShutdownClearsInitializedState)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;

    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));
    NLS::Base::Jobs::ShutdownJobSystem();

    EXPECT_FALSE(NLS::Base::Jobs::IsJobSystemInitialized());
    EXPECT_EQ(NLS::Base::Jobs::GetJobWorkerCount(), 0u);
}

TEST_F(JobSystemSchedulerTests, SingleJobExecutesExactlyOnceAndCompletionClearsHandle)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterJobData data{&counter};
    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = IncrementCounterJob;
    desc.userData = &data;
    desc.debugName = "IncrementCounter";

    auto handle = NLS::Base::Jobs::ScheduleJob(desc);
    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(handle));

    NLS::Base::Jobs::Complete(handle);

    EXPECT_EQ(counter.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(NLS::Base::Jobs::HasBeenSynced(handle));
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));

    NLS::Base::Jobs::Complete(handle);
    EXPECT_EQ(counter.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemSchedulerTests, DependencyRunsBeforeDependentJobObservesData)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<bool> source = false;
    std::atomic<bool> observed = false;

    NLS::Base::Jobs::JobScheduleDesc firstDesc;
    firstDesc.function = SetFlagJob;
    firstDesc.userData = &source;
    auto first = NLS::Base::Jobs::ScheduleJob(firstDesc);

    FlagJobData observeData{&source, &observed};
    NLS::Base::Jobs::JobScheduleDesc secondDesc;
    secondDesc.function = ObserveFlagJob;
    secondDesc.userData = &observeData;
    auto second = NLS::Base::Jobs::ScheduleJobDepends(secondDesc, first);

    NLS::Base::Jobs::Complete(second);

    EXPECT_TRUE(observed.load(std::memory_order_acquire));
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(first));
    EXPECT_TRUE(NLS::Base::Jobs::HasBeenSynced(second));
}

TEST_F(JobSystemSchedulerTests, CompleteNoClearPreservesHandleForRepeatedQueries)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterJobData data{&counter};
    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = IncrementCounterJob;
    desc.userData = &data;

    auto handle = NLS::Base::Jobs::ScheduleJob(desc);
    NLS::Base::Jobs::CompleteNoClear(handle);

    EXPECT_NE(handle.id, 0u);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
    EXPECT_TRUE(NLS::Base::Jobs::HasBeenSynced(handle));
    EXPECT_EQ(counter.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemSchedulerTests, WaitingThreadExecutesQueuedEligibleWork)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterJobData data{&counter};
    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = IncrementCounterJob;
    desc.userData = &data;
    desc.safetyPolicy = NLS::Base::Jobs::JobSafetyPolicy::GuaranteedNoSyncWait;
    auto handle = NLS::Base::Jobs::ScheduleJob(desc);

    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(handle));
    NLS::Base::Jobs::Complete(handle);

    EXPECT_EQ(counter.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemSchedulerTests, ExecuteOneJobPrefersHighPriorityBeforeNormal)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::mutex mutex;
    std::vector<int> order;
    OrderJobData normalData{&mutex, &order, 1};
    OrderJobData highData{&mutex, &order, 2};

    NLS::Base::Jobs::JobScheduleDesc normalDesc;
    normalDesc.function = PushOrderJob;
    normalDesc.userData = &normalData;
    normalDesc.priority = NLS::Base::Jobs::JobPriority::Normal;
    auto normal = NLS::Base::Jobs::ScheduleJob(normalDesc);

    NLS::Base::Jobs::JobScheduleDesc highDesc;
    highDesc.function = PushOrderJob;
    highDesc.userData = &highData;
    highDesc.priority = NLS::Base::Jobs::JobPriority::High;
    auto high = NLS::Base::Jobs::ScheduleJob(highDesc);

    EXPECT_TRUE(NLS::Base::Jobs::ExecuteOneJobQueueJob());
    EXPECT_TRUE(NLS::Base::Jobs::ExecuteOneJobQueueJob());

    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 2);
    EXPECT_EQ(order[1], 1);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(high));
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(normal));
}

TEST_F(JobSystemSchedulerTests, ShutdownImmediateRejectsNewWork)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);

    std::atomic<int> counter = 0;
    CounterJobData data{&counter};
    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = IncrementCounterJob;
    desc.userData = &data;
    auto handle = NLS::Base::Jobs::ScheduleJob(desc);

    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
    EXPECT_EQ(counter.load(std::memory_order_acquire), 0);
}

TEST_F(JobSystemSchedulerTests, ImmediateShutdownDoesNotReportRunningCancelledJobCompleteBeforeItReturns)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> started = 0;
    std::atomic<int> finished = 0;
    std::atomic<bool> release = false;
    BlockingCounterJobData data{&started, &finished, &release};

    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = BlockingCounterJob;
    desc.userData = &data;
    const auto handle = NLS::Base::Jobs::ScheduleJob(desc);
    ASSERT_NE(handle.id, 0u);

    for (int attempt = 0; attempt < 200 && started.load(std::memory_order_acquire) == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(started.load(std::memory_order_acquire), 1);

    std::thread shutdownThread(
        []
        {
            NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(handle));
    EXPECT_EQ(finished.load(std::memory_order_acquire), 0);

    release.store(true, std::memory_order_release);
    shutdownThread.join();

    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
    EXPECT_EQ(finished.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemSchedulerTests, ScheduleRejectsUnknownDependencyHandle)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterJobData data{&counter};
    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = IncrementCounterJob;
    desc.userData = &data;

    const NLS::Base::Jobs::JobHandle invalidDependency{0x1234u, 77u};
    const auto handle = NLS::Base::Jobs::ScheduleJobDepends(desc, invalidDependency);

    EXPECT_EQ(handle.id, 0u);
    EXPECT_EQ(counter.load(std::memory_order_acquire), 0);
}

TEST_F(JobSystemSchedulerTests, CompleteUnknownHandleReturnsWithoutBlocking)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS::Base::Jobs::JobHandle unknown{0x1234u, 77u};

    NLS::Base::Jobs::Complete(unknown);

    EXPECT_EQ(unknown.id, 0u);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(unknown));
}

TEST_F(JobSystemSchedulerTests, WorkerThreadsExecuteQueuedJobs)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 2u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterJobData data{&counter};
    std::vector<NLS::Base::Jobs::JobHandle> handles;
    handles.reserve(32u);

    for (size_t index = 0u; index < 32u; ++index)
    {
        NLS::Base::Jobs::JobScheduleDesc desc;
        desc.function = IncrementCounterJob;
        desc.userData = &data;
        desc.debugName = "WorkerIncrement";
        handles.push_back(NLS::Base::Jobs::ScheduleJob(desc));
    }

    NLS::Base::Jobs::CompleteAll(handles.data(), handles.size());

    EXPECT_EQ(counter.load(std::memory_order_acquire), 32);
    for (const auto& handle : handles)
        EXPECT_TRUE(NLS::Base::Jobs::HasBeenSynced(handle));
}

TEST_F(JobSystemSchedulerTests, CompleteDoesNotRunUnrelatedSyncCapableWork)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> unrelatedCounter = 0;
    CounterJobData unrelatedData{&unrelatedCounter};
    NLS::Base::Jobs::JobScheduleDesc unrelatedDesc;
    unrelatedDesc.function = IncrementCounterJob;
    unrelatedDesc.userData = &unrelatedData;
    auto unrelated = NLS::Base::Jobs::ScheduleJob(unrelatedDesc);

    std::atomic<int> targetCounter = 0;
    CounterJobData targetData{&targetCounter};
    NLS::Base::Jobs::JobScheduleDesc targetDesc;
    targetDesc.function = IncrementCounterJob;
    targetDesc.userData = &targetData;
    auto target = NLS::Base::Jobs::ScheduleJob(targetDesc);

    NLS::Base::Jobs::Complete(target);

    EXPECT_EQ(targetCounter.load(std::memory_order_acquire), 1);
    EXPECT_EQ(unrelatedCounter.load(std::memory_order_acquire), 0);

    NLS::Base::Jobs::Complete(unrelated);
    EXPECT_EQ(unrelatedCounter.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemSchedulerTests, HighPriorityDispatchYieldsToNormalAfterBoundedStreak)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::mutex mutex;
    std::vector<int> order;
    std::vector<OrderJobData> highData;
    highData.reserve(9u);
    std::vector<NLS::Base::Jobs::JobHandle> handles;
    handles.reserve(10u);

    for (int index = 0; index < 9; ++index)
    {
        highData.push_back({&mutex, &order, 100 + index});
        NLS::Base::Jobs::JobScheduleDesc desc;
        desc.function = PushOrderJob;
        desc.userData = &highData.back();
        desc.priority = NLS::Base::Jobs::JobPriority::High;
        handles.push_back(NLS::Base::Jobs::ScheduleJob(desc));
    }

    OrderJobData normalData{&mutex, &order, 1};
    NLS::Base::Jobs::JobScheduleDesc normalDesc;
    normalDesc.function = PushOrderJob;
    normalDesc.userData = &normalData;
    normalDesc.priority = NLS::Base::Jobs::JobPriority::Normal;
    auto normal = NLS::Base::Jobs::ScheduleJob(normalDesc);
    handles.push_back(normal);

    for (size_t index = 0; index < handles.size(); ++index)
        EXPECT_TRUE(NLS::Base::Jobs::ExecuteOneJobQueueJob());

    ASSERT_EQ(order.size(), handles.size());
    EXPECT_EQ(order[8], 1);
}

TEST_F(JobSystemSchedulerTests, FailedDependencyCompletesDependentWithoutRunningIt)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    auto failingDependency = NLS::Base::Jobs::ScheduleJob({ThrowingJob});

    std::atomic<int> counter = 0;
    CounterJobData data{&counter};
    NLS::Base::Jobs::JobScheduleDesc dependentDesc;
    dependentDesc.function = IncrementCounterJob;
    dependentDesc.userData = &data;
    auto dependent = NLS::Base::Jobs::ScheduleJobDepends(dependentDesc, failingDependency);

    std::atomic<bool> completeReturned = false;
    std::thread waiter(
        [&]
        {
            NLS::Base::Jobs::CompleteNoClear(dependent);
            completeReturned.store(true, std::memory_order_release);
        });

    for (int attempt = 0; attempt < 200 && !completeReturned.load(std::memory_order_acquire); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    const bool returnedBeforeShutdown = completeReturned.load(std::memory_order_acquire);
    if (!returnedBeforeShutdown)
        NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);

    waiter.join();

    EXPECT_TRUE(returnedBeforeShutdown);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(failingDependency));
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(dependent));
    EXPECT_EQ(counter.load(std::memory_order_acquire), 0);
}

TEST_F(JobSystemSchedulerTests, FailedDependencyKeepsDependentPendingUntilCancelCleanupReturns)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    auto failingDependency = NLS::Base::Jobs::ScheduleJob({ThrowingJob});

    std::atomic<int> cleanupStarted = 0;
    std::atomic<int> cleanupFinished = 0;
    std::atomic<bool> releaseCleanup = false;
    BlockingCounterJobData cleanupData{&cleanupStarted, &cleanupFinished, &releaseCleanup};

    std::atomic<int> dependentCounter = 0;
    CounterJobData dependentData{&dependentCounter};
    NLS::Base::Jobs::JobScheduleDesc dependentDesc;
    dependentDesc.function = IncrementCounterJob;
    dependentDesc.userData = &dependentData;
    dependentDesc.cancelFunction = BlockingCancel;
    dependentDesc.cancelUserData = &cleanupData;
    auto dependent = NLS::Base::Jobs::ScheduleJobDepends(dependentDesc, failingDependency);

    std::atomic<bool> waitReturned = false;
    std::thread waiter(
        [&]
        {
            NLS::Base::Jobs::CompleteNoClear(dependent);
            waitReturned.store(true, std::memory_order_release);
        });

    for (int attempt = 0; attempt < 200 && cleanupStarted.load(std::memory_order_acquire) == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(cleanupStarted.load(std::memory_order_acquire), 1);

    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(dependent));
    EXPECT_FALSE(waitReturned.load(std::memory_order_acquire));
    EXPECT_EQ(cleanupFinished.load(std::memory_order_acquire), 0);

    releaseCleanup.store(true, std::memory_order_release);
    waiter.join();

    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(failingDependency));
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(dependent));
    EXPECT_TRUE(waitReturned.load(std::memory_order_acquire));
    EXPECT_EQ(cleanupFinished.load(std::memory_order_acquire), 1);
    EXPECT_EQ(dependentCounter.load(std::memory_order_acquire), 0);
}

TEST_F(JobSystemSchedulerTests, ThrowingForegroundJobDoesNotRunCancelCallbackAfterStart)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> cancelCount = 0;
    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = ThrowingJob;
    desc.cancelFunction = CountCancel;
    desc.cancelUserData = &cancelCount;

    auto handle = NLS::Base::Jobs::ScheduleJob(desc);
    ASSERT_NE(handle.id, 0u);

    NLS::Base::Jobs::CompleteNoClear(handle);

    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
    EXPECT_EQ(
        NLS::Base::Jobs::Internal::GetJobCompletionStatus(handle),
        NLS::Base::Jobs::JobCompletionStatus::Failed);
    EXPECT_EQ(cancelCount.load(std::memory_order_acquire), 0);
}

TEST_F(JobSystemSchedulerTests, StaleHandleFromPreviousInitializationDoesNotAliasNewForegroundJob)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> oldCounter = 0;
    CounterJobData oldData{&oldCounter};
    NLS::Base::Jobs::JobScheduleDesc oldDesc;
    oldDesc.function = IncrementCounterJob;
    oldDesc.userData = &oldData;
    const auto staleHandle = NLS::Base::Jobs::ScheduleJob(oldDesc);
    ASSERT_NE(staleHandle.id, 0u);

    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);

    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> newCounter = 0;
    CounterJobData newData{&newCounter};
    NLS::Base::Jobs::JobScheduleDesc newDesc;
    newDesc.function = IncrementCounterJob;
    newDesc.userData = &newData;
    const auto newHandle = NLS::Base::Jobs::ScheduleJob(newDesc);
    ASSERT_NE(newHandle.id, 0u);

    EXPECT_NE(staleHandle.generation, newHandle.generation);
    EXPECT_FALSE(NLS::Base::Jobs::Internal::IsKnownJobHandle(staleHandle));
    EXPECT_TRUE(NLS::Base::Jobs::Internal::IsKnownJobHandle(newHandle));

    auto handleToComplete = newHandle;
    NLS::Base::Jobs::Complete(handleToComplete);
    EXPECT_EQ(newCounter.load(std::memory_order_acquire), 1);
    EXPECT_EQ(oldCounter.load(std::memory_order_acquire), 0);
}

TEST_F(JobSystemSchedulerTests, WorkerCallbackShutdownRequestReturnsWithoutSelfJoin)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<bool> returned = false;
    ShutdownFromJobData data{&returned};
    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = ShutdownFromJob;
    desc.userData = &data;
    auto handle = NLS::Base::Jobs::ScheduleJob(desc);
    ASSERT_NE(handle.id, 0u);

    for (int attempt = 0; attempt < 200 && !returned.load(std::memory_order_acquire); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    EXPECT_TRUE(returned.load(std::memory_order_acquire));
    NLS::Base::Jobs::Complete(handle);
}

TEST(JobSystemSchedulerHeaderContractTests, PublicHeadersCanBeIncludedTogether)
{
    NLS::Base::Jobs::JobHandle handle;
    NLS::Base::Jobs::JobBatchDispatcher dispatcher;
    NLS::Base::Jobs::JobDiagnosticSnapshot snapshot;
    NLS::Base::Jobs::WorkStealingRange range;
    NLS_BindingJobHandle bindingHandle{};

    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
    EXPECT_TRUE(dispatcher.Empty());
    EXPECT_FALSE(snapshot.initialized);
    EXPECT_EQ(range.jobCount, 0);
    EXPECT_EQ(bindingHandle.id, 0u);
}
