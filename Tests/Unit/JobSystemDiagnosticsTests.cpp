#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <type_traits>

#include "Jobs/BackgroundJobQueue.h"
#include "Jobs/JobDiagnostics.h"
#include "Jobs/JobSafety.h"
#include "Jobs/JobSystem.h"

namespace
{
    class JobSystemDiagnosticsTests : public ::testing::Test
    {
    protected:
        void TearDown() override
        {
            NLS::Base::Jobs::ShutdownJobSystem();
            NLS::Base::Jobs::ResetJobDiagnosticsForTesting();
#if defined(NLS_ENABLE_TEST_HOOKS)
            NLS::Base::Jobs::ResetJobSystemForTesting();
#endif
        }
    };

    struct CounterData
    {
        std::atomic<int>* counter = nullptr;
    };

    struct BlockingData
    {
        std::atomic<int>* started = nullptr;
        std::atomic<bool>* release = nullptr;
    };

    struct CompleteOtherHandleData
    {
        NLS::Base::Jobs::JobHandle* handle = nullptr;
        std::atomic<bool>* returned = nullptr;
    };

    void IncrementCounter(void* userData)
    {
        auto* data = static_cast<CounterData*>(userData);
        data->counter->fetch_add(1, std::memory_order_acq_rel);
    }

    void BlockingJob(void* userData)
    {
        auto* data = static_cast<BlockingData*>(userData);
        data->started->store(1, std::memory_order_release);
        while (!data->release->load(std::memory_order_acquire))
            std::this_thread::yield();
    }

    void CompleteOtherHandle(void* userData)
    {
        auto* data = static_cast<CompleteOtherHandleData*>(userData);
        NLS::Base::Jobs::CompleteNoClear(*data->handle);
        data->returned->store(true, std::memory_order_release);
    }

    void NoOpForEach(void*, uint32_t)
    {
    }

    void ThrowingJob(void*)
    {
        throw std::runtime_error("expected diagnostics failure");
    }

    bool HasViolation(
        const NLS::Base::Jobs::JobDiagnosticSnapshot& snapshot,
        const NLS::Base::Jobs::JobViolationKind kind)
    {
        return std::any_of(
            snapshot.recentViolations.begin(),
            snapshot.recentViolations.end(),
            [kind](const NLS::Base::Jobs::JobViolationRecord& record)
            {
                return record.kind == kind;
        });
    }

    bool HasJobState(
        const NLS::Base::Jobs::JobDiagnosticSnapshot& snapshot,
        const char* debugName,
        const NLS::Base::Jobs::JobLifecycleState state)
    {
        return std::any_of(
            snapshot.recentJobs.begin(),
            snapshot.recentJobs.end(),
            [debugName, state](const NLS::Base::Jobs::JobDiagnosticRecord& record)
            {
                return record.debugName == debugName && record.state == state;
            });
    }
}

TEST_F(JobSystemDiagnosticsTests, SnapshotTracksLifecycleCountsAndRecentJobNames)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterData data{&counter};

    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = IncrementCounter;
    desc.userData = &data;
    desc.debugName = "DiagnosticsJob";

    auto handle = NLS::Base::Jobs::ScheduleJob(desc);
    NLS::Base::Jobs::Complete(handle);

    auto snapshot = NLS::Base::Jobs::CopyJobDiagnosticSnapshot();
    EXPECT_TRUE(snapshot.initialized);
    EXPECT_EQ(snapshot.workerCount, 0u);
    EXPECT_GE(snapshot.completedJobCount, 1u);
    ASSERT_FALSE(snapshot.recentJobs.empty());
    EXPECT_TRUE(std::any_of(
        snapshot.recentJobs.begin(),
        snapshot.recentJobs.end(),
        [](const NLS::Base::Jobs::JobDiagnosticRecord& record)
        {
            return record.debugName == "DiagnosticsJob" &&
                record.state == NLS::Base::Jobs::JobLifecycleState::Completed;
        }));
}

TEST_F(JobSystemDiagnosticsTests, DisallowedSyncWaitRecordsViolation)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterData data{&counter};

    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = IncrementCounter;
    desc.userData = &data;

    auto handle = NLS::Base::Jobs::ScheduleJob(desc);
    {
        NLS::Base::Jobs::DisallowJobSyncWaitScope disallow;
        EXPECT_TRUE(NLS::Base::Jobs::IsJobSyncWaitDisallowedForCurrentThread());
        NLS::Base::Jobs::Complete(handle);
    }

    EXPECT_FALSE(NLS::Base::Jobs::IsJobSyncWaitDisallowedForCurrentThread());
    EXPECT_TRUE(HasViolation(
        NLS::Base::Jobs::CopyJobDiagnosticSnapshot(),
        NLS::Base::Jobs::JobViolationKind::SyncWaitDisallowed));
    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
}

TEST_F(JobSystemDiagnosticsTests, DisallowJobSyncWaitScopeIsMoveAndCopyProof)
{
    static_assert(!std::is_copy_constructible_v<NLS::Base::Jobs::DisallowJobSyncWaitScope>);
    static_assert(!std::is_copy_assignable_v<NLS::Base::Jobs::DisallowJobSyncWaitScope>);
    static_assert(!std::is_move_constructible_v<NLS::Base::Jobs::DisallowJobSyncWaitScope>);
    static_assert(!std::is_move_assignable_v<NLS::Base::Jobs::DisallowJobSyncWaitScope>);
}

TEST_F(JobSystemDiagnosticsTests, CompletedHandleDoesNotRecordNoSyncWaitViolation)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterData data{&counter};
    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = IncrementCounter;
    desc.userData = &data;
    auto handle = NLS::Base::Jobs::ScheduleJob(desc);
    NLS::Base::Jobs::CompleteNoClear(handle);

    {
        NLS::Base::Jobs::DisallowJobSyncWaitScope disallow;
        NLS::Base::Jobs::CompleteNoClear(handle);
    }

    EXPECT_FALSE(HasViolation(
        NLS::Base::Jobs::CopyJobDiagnosticSnapshot(),
        NLS::Base::Jobs::JobViolationKind::SyncWaitDisallowed));
}

TEST_F(JobSystemDiagnosticsTests, ThrowingForegroundJobRecordsCallbackExceptionViolation)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = ThrowingJob;
    auto handle = NLS::Base::Jobs::ScheduleJob(desc);
    NLS::Base::Jobs::CompleteNoClear(handle);

    EXPECT_TRUE(HasViolation(
        NLS::Base::Jobs::CopyJobDiagnosticSnapshot(),
        NLS::Base::Jobs::JobViolationKind::CallbackException));
}

TEST_F(JobSystemDiagnosticsTests, ThrowingBackgroundJobRecordsCallbackExceptionViolation)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS::Base::Jobs::BackgroundJobDesc desc;
    desc.function = ThrowingJob;
    auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
    NLS::Base::Jobs::CompleteNoClear(handle);

    EXPECT_TRUE(HasViolation(
        NLS::Base::Jobs::CopyJobDiagnosticSnapshot(),
        NLS::Base::Jobs::JobViolationKind::CallbackException));
}

TEST_F(JobSystemDiagnosticsTests, GuaranteedNoSyncWaitJobCannotSynchronouslyCompleteAnotherHandle)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> targetCounter = 0;
    CounterData targetData{&targetCounter};
    NLS::Base::Jobs::JobScheduleDesc targetDesc;
    targetDesc.function = IncrementCounter;
    targetDesc.userData = &targetData;
    auto target = NLS::Base::Jobs::ScheduleJob(targetDesc);

    std::atomic<bool> waiterReturned = false;
    CompleteOtherHandleData waiterData{&target, &waiterReturned};
    NLS::Base::Jobs::JobScheduleDesc waiterDesc;
    waiterDesc.function = CompleteOtherHandle;
    waiterDesc.userData = &waiterData;
    waiterDesc.priority = NLS::Base::Jobs::JobPriority::High;
    waiterDesc.safetyPolicy = NLS::Base::Jobs::JobSafetyPolicy::GuaranteedNoSyncWait;
    auto waiter = NLS::Base::Jobs::ScheduleJob(waiterDesc);

    NLS::Base::Jobs::Complete(waiter);

    EXPECT_TRUE(waiterReturned.load(std::memory_order_acquire));
    EXPECT_EQ(targetCounter.load(std::memory_order_acquire), 0);
    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(target));
    EXPECT_TRUE(HasViolation(
        NLS::Base::Jobs::CopyJobDiagnosticSnapshot(),
        NLS::Base::Jobs::JobViolationKind::SyncWaitDisallowed));
    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
}

TEST_F(JobSystemDiagnosticsTests, GuaranteedNoSyncWaitCombineCannotSynchronouslyCompleteAnotherHandle)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> targetCounter = 0;
    CounterData targetData{&targetCounter};
    NLS::Base::Jobs::JobScheduleDesc targetDesc;
    targetDesc.function = IncrementCounter;
    targetDesc.userData = &targetData;
    auto target = NLS::Base::Jobs::ScheduleJob(targetDesc);

    std::atomic<bool> combineReturned = false;
    CompleteOtherHandleData combineData{&target, &combineReturned};
    NLS::Base::Jobs::JobForEachDesc forEachDesc;
    forEachDesc.function = NoOpForEach;
    forEachDesc.userData = &combineData;
    forEachDesc.iterationCount = 1u;
    forEachDesc.combineFunction = CompleteOtherHandle;
    forEachDesc.priority = NLS::Base::Jobs::JobPriority::High;
    forEachDesc.safetyPolicy = NLS::Base::Jobs::JobSafetyPolicy::GuaranteedNoSyncWait;
    auto combine = NLS::Base::Jobs::ScheduleJobForEach(forEachDesc);

    NLS::Base::Jobs::Complete(combine);

    EXPECT_TRUE(combineReturned.load(std::memory_order_acquire));
    EXPECT_EQ(targetCounter.load(std::memory_order_acquire), 0);
    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(target));
    EXPECT_TRUE(HasViolation(
        NLS::Base::Jobs::CopyJobDiagnosticSnapshot(),
        NLS::Base::Jobs::JobViolationKind::SyncWaitDisallowed));
    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
}

TEST_F(JobSystemDiagnosticsTests, ClearWithoutSyncRecordsOwnershipHandoffViolation)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterData data{&counter};

    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = IncrementCounter;
    desc.userData = &data;

    auto handle = NLS::Base::Jobs::ScheduleJob(desc);
    NLS::Base::Jobs::ClearWithoutSync(handle);

    EXPECT_TRUE(NLS::Base::Jobs::HasBeenSynced(handle));
    EXPECT_TRUE(HasViolation(
        NLS::Base::Jobs::CopyJobDiagnosticSnapshot(),
        NLS::Base::Jobs::JobViolationKind::ClearedWithoutSync));

    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
}

TEST_F(JobSystemDiagnosticsTests, RejectedSchedulePathsRecordStructuredViolations)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS::Base::Jobs::JobScheduleDesc nullDesc;
    auto nullHandle = NLS::Base::Jobs::ScheduleJob(nullDesc);
    EXPECT_EQ(nullHandle.id, 0u);

    std::atomic<int> counter = 0;
    CounterData data{&counter};
    NLS::Base::Jobs::JobScheduleDesc validDesc;
    validDesc.function = IncrementCounter;
    validDesc.userData = &data;
    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
    auto rejectedHandle = NLS::Base::Jobs::ScheduleJob(validDesc);
    EXPECT_EQ(rejectedHandle.id, 0u);

    const auto snapshot = NLS::Base::Jobs::CopyJobDiagnosticSnapshot();
    EXPECT_TRUE(HasViolation(snapshot, NLS::Base::Jobs::JobViolationKind::NullCallback));
    EXPECT_TRUE(HasViolation(snapshot, NLS::Base::Jobs::JobViolationKind::ShutdownSchedulingRejected));
}

TEST_F(JobSystemDiagnosticsTests, WaitingDependencyStateIsRecorded)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterData data{&counter};
    NLS::Base::Jobs::JobScheduleDesc dependencyDesc;
    dependencyDesc.function = IncrementCounter;
    dependencyDesc.userData = &data;
    const auto dependency = NLS::Base::Jobs::ScheduleJob(dependencyDesc);

    NLS::Base::Jobs::JobScheduleDesc dependentDesc;
    dependentDesc.function = IncrementCounter;
    dependentDesc.userData = &data;
    dependentDesc.dependency = dependency;
    dependentDesc.debugName = "WaitingDiagnosticsJob";
    const auto dependent = NLS::Base::Jobs::ScheduleJob(dependentDesc);
    ASSERT_NE(dependent.id, 0u);

    EXPECT_TRUE(HasJobState(
        NLS::Base::Jobs::CopyJobDiagnosticSnapshot(),
        "WaitingDiagnosticsJob",
        NLS::Base::Jobs::JobLifecycleState::WaitingForDependencies));

    auto handleToComplete = dependent;
    NLS::Base::Jobs::Complete(handleToComplete);
}

TEST_F(JobSystemDiagnosticsTests, WaitingDependencyCancellationDoesNotDecrementUnrelatedQueuedCount)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> queuedCounter = 0;
    CounterData queuedData{&queuedCounter};
    NLS::Base::Jobs::JobScheduleDesc queuedDesc;
    queuedDesc.function = IncrementCounter;
    queuedDesc.userData = &queuedData;
    queuedDesc.debugName = "StillQueuedDiagnosticsJob";
    auto queued = NLS::Base::Jobs::ScheduleJob(queuedDesc);
    ASSERT_NE(queued.id, 0u);

    NLS::Base::Jobs::JobScheduleDesc dependencyDesc;
    dependencyDesc.function = ThrowingJob;
    dependencyDesc.debugName = "FailingDiagnosticsDependency";
    auto dependency = NLS::Base::Jobs::ScheduleJob(dependencyDesc);
    ASSERT_NE(dependency.id, 0u);

    std::atomic<int> dependentCounter = 0;
    CounterData dependentData{&dependentCounter};
    NLS::Base::Jobs::JobScheduleDesc dependentDesc;
    dependentDesc.function = IncrementCounter;
    dependentDesc.userData = &dependentData;
    dependentDesc.dependency = dependency;
    dependentDesc.debugName = "CancelledWaitingDiagnosticsJob";
    auto dependent = NLS::Base::Jobs::ScheduleJob(dependentDesc);
    ASSERT_NE(dependent.id, 0u);

    NLS::Base::Jobs::CompleteNoClear(dependency);

    const auto snapshot = NLS::Base::Jobs::CopyJobDiagnosticSnapshot();
    EXPECT_EQ(snapshot.queuedJobCount, 1u);
    EXPECT_EQ(snapshot.runningJobCount, 0u);
    EXPECT_EQ(dependentCounter.load(std::memory_order_acquire), 0);
    EXPECT_TRUE(HasJobState(
        snapshot,
        "CancelledWaitingDiagnosticsJob",
        NLS::Base::Jobs::JobLifecycleState::Cancelled));

    NLS::Base::Jobs::Complete(queued);
}

TEST_F(JobSystemDiagnosticsTests, RunningJobCancelledByImmediateShutdownDoesNotLeaveRunningCount)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 1u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> started = 0;
    std::atomic<bool> release = false;
    BlockingData data{&started, &release};
    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = BlockingJob;
    desc.userData = &data;
    desc.debugName = "RunningCancelDiagnosticsJob";
    auto handle = NLS::Base::Jobs::ScheduleJob(desc);
    ASSERT_NE(handle.id, 0u);

    const auto startDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (started.load(std::memory_order_acquire) == 0 &&
        std::chrono::steady_clock::now() < startDeadline)
    {
        std::this_thread::yield();
    }
    ASSERT_EQ(started.load(std::memory_order_acquire), 1);

    std::thread shutdownThread(
        []
        {
            NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
        });

    NLS::Base::Jobs::JobDiagnosticSnapshot snapshot;
    const auto cancelDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do
    {
        snapshot = NLS::Base::Jobs::CopyJobDiagnosticSnapshot();
        if (HasJobState(
                snapshot,
                "RunningCancelDiagnosticsJob",
                NLS::Base::Jobs::JobLifecycleState::Cancelled))
        {
            break;
        }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < cancelDeadline);

    EXPECT_TRUE(HasJobState(
        snapshot,
        "RunningCancelDiagnosticsJob",
        NLS::Base::Jobs::JobLifecycleState::Cancelled));
    EXPECT_EQ(snapshot.runningJobCount, 0u);

    release.store(true, std::memory_order_release);
    shutdownThread.join();
}

TEST_F(JobSystemDiagnosticsTests, RejectedBackgroundSchedulePathsRecordStructuredViolations)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS::Base::Jobs::BackgroundJobDesc nullJobDesc;
    auto nullBackgroundHandle = NLS::Base::Jobs::ScheduleBackgroundJob(nullJobDesc);
    EXPECT_EQ(nullBackgroundHandle.id, 0u);

    NLS::Base::Jobs::MainThreadContinuationDesc nullContinuationDesc;
    EXPECT_FALSE(NLS::Base::Jobs::ScheduleMainThreadContinuation(nullContinuationDesc));

    std::atomic<int> counter = 0;
    CounterData data{&counter};
    NLS::Base::Jobs::BackgroundJobDesc validBackgroundDesc;
    validBackgroundDesc.function = IncrementCounter;
    validBackgroundDesc.userData = &data;
    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);

    auto rejectedBackgroundHandle = NLS::Base::Jobs::ScheduleBackgroundJob(validBackgroundDesc);
    EXPECT_EQ(rejectedBackgroundHandle.id, 0u);

    NLS::Base::Jobs::MainThreadContinuationDesc validContinuationDesc;
    validContinuationDesc.function = [](void*) {};
    EXPECT_FALSE(NLS::Base::Jobs::ScheduleMainThreadContinuation(validContinuationDesc));

    const auto snapshot = NLS::Base::Jobs::CopyJobDiagnosticSnapshot();
    EXPECT_TRUE(HasViolation(snapshot, NLS::Base::Jobs::JobViolationKind::NullCallback));
    EXPECT_TRUE(HasViolation(snapshot, NLS::Base::Jobs::JobViolationKind::ShutdownSchedulingRejected));
}

TEST_F(JobSystemDiagnosticsTests, StaleHandleQueriesRecordStructuredViolation)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterData data{&counter};
    NLS::Base::Jobs::JobScheduleDesc desc;
    desc.function = IncrementCounter;
    desc.userData = &data;
    const auto staleHandle = NLS::Base::Jobs::ScheduleJob(desc);
    ASSERT_NE(staleHandle.id, 0u);

    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    EXPECT_FALSE(NLS::Base::Jobs::Internal::IsKnownJobHandle(staleHandle));
    NLS::Base::Jobs::CompleteNoClear(staleHandle);

    EXPECT_TRUE(HasViolation(
        NLS::Base::Jobs::CopyJobDiagnosticSnapshot(),
        NLS::Base::Jobs::JobViolationKind::StaleHandle));
}

TEST_F(JobSystemDiagnosticsTests, StaleBackgroundHandleQueriesRecordStructuredViolation)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterData data{&counter};
    NLS::Base::Jobs::BackgroundJobDesc desc;
    desc.function = IncrementCounter;
    desc.userData = &data;
    const auto staleHandle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
    ASSERT_NE(staleHandle.id, 0u);

    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    EXPECT_FALSE(NLS::Base::Jobs::Internal::IsKnownJobHandle(staleHandle));
    (void)NLS::Base::Jobs::IsCompleted(staleHandle);

    EXPECT_TRUE(HasViolation(
        NLS::Base::Jobs::CopyJobDiagnosticSnapshot(),
        NLS::Base::Jobs::JobViolationKind::StaleHandle));
}

TEST_F(JobSystemDiagnosticsTests, ProfilerThreadNameSourcesAreRecordedForBackgroundJobs)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterData data{&counter};

    NLS::Base::Jobs::BackgroundJobDesc desc;
    desc.function = IncrementCounter;
    desc.userData = &data;
    desc.debugName = "BackgroundDiagnosticsJob";

    auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
    NLS::Base::Jobs::Complete(handle);

    const auto snapshot = NLS::Base::Jobs::CopyJobDiagnosticSnapshot();
    EXPECT_TRUE(std::any_of(
        snapshot.recentJobs.begin(),
        snapshot.recentJobs.end(),
        [](const NLS::Base::Jobs::JobDiagnosticRecord& record)
        {
            return record.debugName == "BackgroundDiagnosticsJob" &&
                record.workerName.find("Background Job Worker") != std::string::npos;
        }));
}
