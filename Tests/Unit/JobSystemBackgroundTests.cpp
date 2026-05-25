#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Jobs/BackgroundJobQueue.h"
#include "Jobs/JobBatchDispatcher.h"
#include "Jobs/JobDiagnostics.h"
#include "Jobs/JobSystem.h"
#include "Profiling/Profiler.h"

namespace
{
    class JobSystemBackgroundTests : public ::testing::Test
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

    struct AtomicCounterData
    {
        std::atomic<int>* counter = nullptr;
    };

    void IncrementCounter(void* userData)
    {
        auto* data = static_cast<AtomicCounterData*>(userData);
        data->counter->fetch_add(1, std::memory_order_acq_rel);
    }

    void ThrowingJob(void*)
    {
        throw std::runtime_error("expected cross-queue dependency failure");
    }

    struct ContinuationData
    {
        std::vector<int>* events = nullptr;
        int value = 0;
    };

    void RecordContinuation(void* userData)
    {
        auto* data = static_cast<ContinuationData*>(userData);
        data->events->push_back(data->value);
    }

    std::string ReadRepoFile(const std::filesystem::path& relativePath)
    {
        const auto absolutePath = std::filesystem::path(NLS_ROOT_DIR) / relativePath;
        std::ifstream input(absolutePath, std::ios::binary);
        if (!input)
            return {};

        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

    struct BlockingJobData
    {
        std::atomic<int>* started = nullptr;
        std::atomic<int>* finished = nullptr;
        std::atomic<bool>* release = nullptr;
    };

    void BlockingBackgroundJob(void* userData)
    {
        auto* data = static_cast<BlockingJobData*>(userData);
        data->started->fetch_add(1, std::memory_order_acq_rel);
        while (!data->release->load(std::memory_order_acquire))
            std::this_thread::yield();
        data->finished->fetch_add(1, std::memory_order_acq_rel);
    }

    void BlockingBackgroundCancel(void* userData)
    {
        auto* data = static_cast<BlockingJobData*>(userData);
        data->started->fetch_add(1, std::memory_order_acq_rel);
        while (!data->release->load(std::memory_order_acquire))
            std::this_thread::yield();
        data->finished->fetch_add(1, std::memory_order_acq_rel);
    }

    struct FlagJobData
    {
        std::atomic<bool>* source = nullptr;
        std::atomic<bool>* observed = nullptr;
    };

    void SetFlagAfterRelease(void* userData)
    {
        auto* data = static_cast<BlockingJobData*>(userData);
        data->started->fetch_add(1, std::memory_order_acq_rel);
        while (!data->release->load(std::memory_order_acquire))
            std::this_thread::yield();
        data->finished->fetch_add(1, std::memory_order_acq_rel);
    }

    void ObserveFlag(void* userData)
    {
        auto* data = static_cast<FlagJobData*>(userData);
        data->observed->store(data->source->load(std::memory_order_acquire), std::memory_order_release);
    }

    struct ReleaseFlagData
    {
        std::atomic<bool>* release = nullptr;
        std::atomic<bool>* flag = nullptr;
    };

    void SetFlagAfterReleaseJob(void* userData)
    {
        auto* data = static_cast<ReleaseFlagData*>(userData);
        while (!data->release->load(std::memory_order_acquire))
            std::this_thread::yield();
        data->flag->store(true, std::memory_order_release);
    }

    void CountCancel(void* userData)
    {
        auto* counter = static_cast<std::atomic<int>*>(userData);
        counter->fetch_add(1, std::memory_order_acq_rel);
    }

    void ThrowingCancel(void*)
    {
        throw std::runtime_error("expected cancel failure");
    }

    void ThrowingBackgroundJob(void*)
    {
        throw std::runtime_error("expected background failure");
    }

    void ThrowingContinuation(void*)
    {
        throw std::runtime_error("expected continuation failure");
    }

    struct CompleteBackgroundFromCancelData
    {
        NLS::Base::Jobs::JobHandle* background = nullptr;
        std::atomic<bool>* returned = nullptr;
    };

    void CompleteBackgroundFromCancel(void* userData)
    {
        auto* data = static_cast<CompleteBackgroundFromCancelData*>(userData);
        NLS::Base::Jobs::CompleteNoClear(*data->background);
        data->returned->store(true, std::memory_order_release);
    }

    void BlockingCancel(void* userData)
    {
        auto* data = static_cast<BlockingJobData*>(userData);
        data->started->fetch_add(1, std::memory_order_acq_rel);
        while (!data->release->load(std::memory_order_acquire))
            std::this_thread::yield();
        data->finished->fetch_add(1, std::memory_order_acq_rel);
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

    struct CompleteForegroundFromBackgroundData
    {
        NLS::Base::Jobs::JobHandle* foreground = nullptr;
        std::atomic<bool>* returned = nullptr;
    };

    void CompleteForegroundFromBackground(void* userData)
    {
        auto* data = static_cast<CompleteForegroundFromBackgroundData*>(userData);
        NLS::Base::Jobs::CompleteNoClear(*data->foreground);
        data->returned->store(true, std::memory_order_release);
    }

    struct CompleteBackgroundFromForegroundData
    {
        NLS::Base::Jobs::JobHandle* background = nullptr;
        std::atomic<bool>* backgroundReady = nullptr;
        std::atomic<bool>* returned = nullptr;
    };

    void CompleteBackgroundFromForeground(void* userData)
    {
        auto* data = static_cast<CompleteBackgroundFromForegroundData*>(userData);
        while (!data->backgroundReady->load(std::memory_order_acquire))
            std::this_thread::yield();

        NLS::Base::Jobs::CompleteNoClear(*data->background);
        data->returned->store(true, std::memory_order_release);
    }

    struct CompleteBackgroundFromHelpedForegroundData
    {
        NLS::Base::Jobs::JobHandle* background = nullptr;
        std::atomic<bool>* returned = nullptr;
    };

    void CompleteBackgroundFromHelpedForeground(void* userData)
    {
        auto* data = static_cast<CompleteBackgroundFromHelpedForegroundData*>(userData);
        NLS::Base::Jobs::CompleteNoClear(*data->background);
        data->returned->store(true, std::memory_order_release);
    }

    struct CompleteBackgroundFromBackgroundData
    {
        NLS::Base::Jobs::JobHandle* background = nullptr;
        std::atomic<bool>* ready = nullptr;
        std::atomic<bool>* returned = nullptr;
    };

    void CompleteBackgroundFromBackground(void* userData)
    {
        auto* data = static_cast<CompleteBackgroundFromBackgroundData*>(userData);
        while (!data->ready->load(std::memory_order_acquire))
            std::this_thread::yield();

        NLS::Base::Jobs::CompleteNoClear(*data->background);
        data->returned->store(true, std::memory_order_release);
    }
}

TEST_F(JobSystemBackgroundTests, BackgroundJobExecutesAndCompletesThroughHandle)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    AtomicCounterData data{&counter};

    NLS::Base::Jobs::BackgroundJobDesc desc;
    desc.function = IncrementCounter;
    desc.userData = &data;

    auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
    NLS::Base::Jobs::Complete(handle);

    EXPECT_EQ(counter.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(NLS::Base::Jobs::HasBeenSynced(handle));
}

TEST_F(JobSystemBackgroundTests, BackgroundJobDebugNameIsOwnedAfterScheduleReturns)
{
    NLS::Base::Profiling::Profiler::ResetForTesting();
    RecordingProfilerDestination destination;
    NLS::Base::Profiling::Profiler::RegisterDestination(destination);
    NLS::Base::Profiling::Profiler::SetEnabled(true);

    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 0u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    AtomicCounterData data{&counter};
    NLS::Base::Jobs::BackgroundJobDesc desc;
    desc.function = IncrementCounter;
    desc.userData = &data;
    std::string debugName = "OwnedBackgroundDebugName";
    desc.debugName = debugName.c_str();
    auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
    ASSERT_NE(handle.id, 0u);
    debugName.assign(debugName.size(), 'x');

    NLS::Base::Jobs::Complete(handle);

    const auto snapshot = NLS::Base::Jobs::CopyJobDiagnosticSnapshot();
    EXPECT_TRUE(std::any_of(
        snapshot.recentJobs.begin(),
        snapshot.recentJobs.end(),
        [](const NLS::Base::Jobs::JobDiagnosticRecord& record)
        {
            return record.debugName == "OwnedBackgroundDebugName";
        }));

    {
        std::lock_guard lock(destination.mutex);
        EXPECT_TRUE(std::any_of(
            destination.events.begin(),
            destination.events.end(),
            [](const RecordedProfilerScope& event)
            {
                return event.phase == "begin" && event.name == "OwnedBackgroundDebugName";
            }));
    }
    EXPECT_EQ(counter.load(std::memory_order_acquire), 1);
    NLS::Base::Profiling::Profiler::ResetForTesting();
}

TEST_F(JobSystemBackgroundTests, BackgroundWorkerRejectsSynchronousWaitForBackgroundJob)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> dependencyCounter = 0;
    AtomicCounterData dependencyData{&dependencyCounter};
    NLS::Base::Jobs::JobScheduleDesc dependencyDesc;
    dependencyDesc.function = IncrementCounter;
    dependencyDesc.userData = &dependencyData;
    auto dependency = NLS::Base::Jobs::ScheduleJob(dependencyDesc);
    ASSERT_NE(dependency.id, 0u);

    std::atomic<int> targetCounter = 0;
    AtomicCounterData targetData{&targetCounter};
    NLS::Base::Jobs::BackgroundJobDesc targetDesc;
    targetDesc.function = IncrementCounter;
    targetDesc.userData = &targetData;
    targetDesc.dependency = dependency;
    auto target = NLS::Base::Jobs::ScheduleBackgroundJob(targetDesc);
    ASSERT_NE(target.id, 0u);

    std::atomic<bool> ready = false;
    std::atomic<bool> returned = false;
    CompleteBackgroundFromBackgroundData waiterData{&target, &ready, &returned};
    NLS::Base::Jobs::BackgroundJobDesc waiterDesc;
    waiterDesc.function = CompleteBackgroundFromBackground;
    waiterDesc.userData = &waiterData;
    auto waiter = NLS::Base::Jobs::ScheduleBackgroundJob(waiterDesc);
    ASSERT_NE(waiter.id, 0u);

    ready.store(true, std::memory_order_release);
    for (int attempt = 0; attempt < 200 && !returned.load(std::memory_order_acquire); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    EXPECT_TRUE(returned.load(std::memory_order_acquire));
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(waiter));
    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(target));
    EXPECT_EQ(targetCounter.load(std::memory_order_acquire), 0);

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

TEST_F(JobSystemBackgroundTests, BackgroundWorkerRejectsForegroundWaitWhenDependencyChainContainsPendingBackground)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 2u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> gateStarted = 0;
    std::atomic<int> gateFinished = 0;
    std::atomic<bool> releaseGate = false;
    BlockingJobData gateData{&gateStarted, &gateFinished, &releaseGate};
    NLS::Base::Jobs::JobScheduleDesc gateDesc;
    gateDesc.function = BlockingBackgroundJob;
    gateDesc.userData = &gateData;
    auto gate = NLS::Base::Jobs::ScheduleJob(gateDesc);
    ASSERT_NE(gate.id, 0u);

    std::atomic<int> backgroundCounter = 0;
    AtomicCounterData backgroundData{&backgroundCounter};
    NLS::Base::Jobs::BackgroundJobDesc backgroundDesc;
    backgroundDesc.function = IncrementCounter;
    backgroundDesc.userData = &backgroundData;
    backgroundDesc.dependency = gate;
    auto background = NLS::Base::Jobs::ScheduleBackgroundJob(backgroundDesc);
    ASSERT_NE(background.id, 0u);

    std::atomic<int> foregroundCounter = 0;
    AtomicCounterData foregroundData{&foregroundCounter};
    NLS::Base::Jobs::JobScheduleDesc foregroundDesc;
    foregroundDesc.function = IncrementCounter;
    foregroundDesc.userData = &foregroundData;
    foregroundDesc.dependency = background;
    auto foreground = NLS::Base::Jobs::ScheduleJob(foregroundDesc);
    ASSERT_NE(foreground.id, 0u);

    std::atomic<bool> returned = false;
    CompleteForegroundFromBackgroundData waiterData{&foreground, &returned};
    NLS::Base::Jobs::BackgroundJobDesc waiterDesc;
    waiterDesc.function = CompleteForegroundFromBackground;
    waiterDesc.userData = &waiterData;
    auto waiter = NLS::Base::Jobs::ScheduleBackgroundJob(waiterDesc);
    ASSERT_NE(waiter.id, 0u);

    for (int attempt = 0; attempt < 200 && !returned.load(std::memory_order_acquire); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    const bool returnedBeforeGateRelease = returned.load(std::memory_order_acquire);
    EXPECT_TRUE(returnedBeforeGateRelease);
    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(foreground));
    EXPECT_EQ(backgroundCounter.load(std::memory_order_acquire), 0);
    EXPECT_EQ(foregroundCounter.load(std::memory_order_acquire), 0);

    releaseGate.store(true, std::memory_order_release);
    NLS::Base::Jobs::CompleteNoClear(gate);
    NLS::Base::Jobs::CompleteNoClear(background);
    NLS::Base::Jobs::CompleteNoClear(foreground);
    NLS::Base::Jobs::CompleteNoClear(waiter);

    const auto snapshot = NLS::Base::Jobs::CopyJobDiagnosticSnapshot();
    EXPECT_TRUE(std::any_of(
        snapshot.recentViolations.begin(),
        snapshot.recentViolations.end(),
        [](const NLS::Base::Jobs::JobViolationRecord& record)
        {
            return record.kind == NLS::Base::Jobs::JobViolationKind::SyncWaitDisallowed;
        }));
}

TEST_F(JobSystemBackgroundTests, BackgroundWorkerRejectsForegroundWaitWhenTransitiveDependencyContainsPendingBackground)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 2u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> gateStarted = 0;
    std::atomic<int> gateFinished = 0;
    std::atomic<bool> releaseGate = false;
    BlockingJobData gateData{&gateStarted, &gateFinished, &releaseGate};
    NLS::Base::Jobs::JobScheduleDesc gateDesc;
    gateDesc.function = BlockingBackgroundJob;
    gateDesc.userData = &gateData;
    auto gate = NLS::Base::Jobs::ScheduleJob(gateDesc);
    ASSERT_NE(gate.id, 0u);

    std::atomic<int> backgroundCounter = 0;
    AtomicCounterData backgroundData{&backgroundCounter};
    NLS::Base::Jobs::BackgroundJobDesc backgroundDesc;
    backgroundDesc.function = IncrementCounter;
    backgroundDesc.userData = &backgroundData;
    backgroundDesc.dependency = gate;
    auto background = NLS::Base::Jobs::ScheduleBackgroundJob(backgroundDesc);
    ASSERT_NE(background.id, 0u);

    std::atomic<int> middleCounter = 0;
    AtomicCounterData middleData{&middleCounter};
    NLS::Base::Jobs::JobScheduleDesc middleDesc;
    middleDesc.function = IncrementCounter;
    middleDesc.userData = &middleData;
    middleDesc.dependency = background;
    auto middle = NLS::Base::Jobs::ScheduleJob(middleDesc);
    ASSERT_NE(middle.id, 0u);

    std::atomic<int> targetCounter = 0;
    AtomicCounterData targetData{&targetCounter};
    NLS::Base::Jobs::JobScheduleDesc targetDesc;
    targetDesc.function = IncrementCounter;
    targetDesc.userData = &targetData;
    targetDesc.dependency = middle;
    auto target = NLS::Base::Jobs::ScheduleJob(targetDesc);
    ASSERT_NE(target.id, 0u);

    std::atomic<bool> returned = false;
    CompleteForegroundFromBackgroundData waiterData{&target, &returned};
    NLS::Base::Jobs::BackgroundJobDesc waiterDesc;
    waiterDesc.function = CompleteForegroundFromBackground;
    waiterDesc.userData = &waiterData;
    auto waiter = NLS::Base::Jobs::ScheduleBackgroundJob(waiterDesc);
    ASSERT_NE(waiter.id, 0u);

    for (int attempt = 0; attempt < 200 && !returned.load(std::memory_order_acquire); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    const bool returnedBeforeGateRelease = returned.load(std::memory_order_acquire);
    EXPECT_TRUE(returnedBeforeGateRelease);
    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(target));
    EXPECT_EQ(backgroundCounter.load(std::memory_order_acquire), 0);
    EXPECT_EQ(middleCounter.load(std::memory_order_acquire), 0);
    EXPECT_EQ(targetCounter.load(std::memory_order_acquire), 0);

    releaseGate.store(true, std::memory_order_release);
    NLS::Base::Jobs::CompleteNoClear(gate);
    NLS::Base::Jobs::CompleteNoClear(background);
    NLS::Base::Jobs::CompleteNoClear(middle);
    NLS::Base::Jobs::CompleteNoClear(target);
    NLS::Base::Jobs::CompleteNoClear(waiter);

    const auto snapshot = NLS::Base::Jobs::CopyJobDiagnosticSnapshot();
    EXPECT_TRUE(std::any_of(
        snapshot.recentViolations.begin(),
        snapshot.recentViolations.end(),
        [](const NLS::Base::Jobs::JobViolationRecord& record)
        {
            return record.kind == NLS::Base::Jobs::JobViolationKind::SyncWaitDisallowed;
        }));
}

TEST_F(JobSystemBackgroundTests, BackgroundJobFailureRunsCancelCleanupOnce)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> cancelCount = 0;
    NLS::Base::Jobs::BackgroundJobDesc desc;
    desc.function = ThrowingJob;
    desc.cancelFunction = CountCancel;
    desc.cancelUserData = &cancelCount;

    auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
    NLS::Base::Jobs::Complete(handle);

    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
    EXPECT_EQ(cancelCount.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemBackgroundTests, BackgroundJobFailureKeepsHandlePendingUntilCancelCleanupReturns)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> cleanupStarted = 0;
    std::atomic<int> cleanupFinished = 0;
    std::atomic<bool> releaseCleanup = false;
    BlockingJobData cleanupData{&cleanupStarted, &cleanupFinished, &releaseCleanup};

    NLS::Base::Jobs::BackgroundJobDesc desc;
    desc.function = ThrowingJob;
    desc.cancelFunction = BlockingBackgroundCancel;
    desc.cancelUserData = &cleanupData;

    auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
    ASSERT_NE(handle.id, 0u);

    std::atomic<bool> waitReturned = false;
    std::thread waiter(
        [&]
        {
            NLS::Base::Jobs::CompleteNoClear(handle);
            waitReturned.store(true, std::memory_order_release);
        });

    for (int attempt = 0; attempt < 200 && cleanupStarted.load(std::memory_order_acquire) == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(cleanupStarted.load(std::memory_order_acquire), 1);

    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(handle));
    EXPECT_FALSE(waitReturned.load(std::memory_order_acquire));
    EXPECT_EQ(cleanupFinished.load(std::memory_order_acquire), 0);

    releaseCleanup.store(true, std::memory_order_release);
    waiter.join();

    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
    EXPECT_TRUE(waitReturned.load(std::memory_order_acquire));
    EXPECT_EQ(cleanupFinished.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemBackgroundTests, BackgroundFailureCancelCallbackRetainsBackgroundWorkerIdentity)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS::Base::Jobs::JobBatchDispatcher gateDispatcher;
    std::atomic<int> gateCounter = 0;
    AtomicCounterData gateData{&gateCounter};
    NLS::Base::Jobs::JobScheduleDesc gateDesc;
    gateDesc.function = IncrementCounter;
    gateDesc.userData = &gateData;
    auto gate = gateDispatcher.AddJob(gateDesc);
    ASSERT_NE(gate.id, 0u);

    std::atomic<int> blockedCounter = 0;
    AtomicCounterData blockedData{&blockedCounter};
    NLS::Base::Jobs::BackgroundJobDesc blockedDesc;
    blockedDesc.function = IncrementCounter;
    blockedDesc.userData = &blockedData;
    blockedDesc.dependency = gate;
    auto blockedBackground = NLS::Base::Jobs::ScheduleBackgroundJob(blockedDesc);
    ASSERT_NE(blockedBackground.id, 0u);

    std::atomic<bool> cancelReturned = false;
    CompleteBackgroundFromCancelData cancelData{&blockedBackground, &cancelReturned};
    NLS::Base::Jobs::BackgroundJobDesc failingDesc;
    failingDesc.function = ThrowingBackgroundJob;
    failingDesc.cancelFunction = CompleteBackgroundFromCancel;
    failingDesc.cancelUserData = &cancelData;
    auto failing = NLS::Base::Jobs::ScheduleBackgroundJob(failingDesc);
    ASSERT_NE(failing.id, 0u);

    for (int attempt = 0; attempt < 200 && !cancelReturned.load(std::memory_order_acquire); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    ASSERT_TRUE(cancelReturned.load(std::memory_order_acquire));
    EXPECT_EQ(gateCounter.load(std::memory_order_acquire), 0);
    EXPECT_EQ(blockedCounter.load(std::memory_order_acquire), 0);
    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(blockedBackground));

    const auto snapshot = NLS::Base::Jobs::CopyJobDiagnosticSnapshot();
    EXPECT_TRUE(std::any_of(
        snapshot.recentViolations.begin(),
        snapshot.recentViolations.end(),
        [](const NLS::Base::Jobs::JobViolationRecord& record)
        {
            return record.kind == NLS::Base::Jobs::JobViolationKind::SyncWaitDisallowed;
        }));

    NLS::Base::Jobs::CompleteNoClear(failing);
    NLS::Base::Jobs::CompleteNoClear(gate);
    NLS::Base::Jobs::CompleteNoClear(blockedBackground);
}

TEST_F(JobSystemBackgroundTests, ImmediateShutdownKeepsQueuedBackgroundJobPendingUntilCancelCleanupReturns)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> dependencyCounter = 0;
    AtomicCounterData dependencyData{&dependencyCounter};
    NLS::Base::Jobs::JobScheduleDesc dependencyDesc;
    dependencyDesc.function = IncrementCounter;
    dependencyDesc.userData = &dependencyData;
    auto dependency = NLS::Base::Jobs::ScheduleJob(dependencyDesc);
    ASSERT_NE(dependency.id, 0u);

    std::atomic<int> cleanupStarted = 0;
    std::atomic<int> cleanupFinished = 0;
    std::atomic<bool> releaseCleanup = false;
    BlockingJobData cleanupData{&cleanupStarted, &cleanupFinished, &releaseCleanup};
    NLS::Base::Jobs::BackgroundJobDesc desc;
    desc.function = IncrementCounter;
    desc.userData = &dependencyData;
    desc.dependency = dependency;
    desc.cancelFunction = BlockingBackgroundCancel;
    desc.cancelUserData = &cleanupData;
    auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
    ASSERT_NE(handle.id, 0u);

    std::atomic<bool> shutdownReturned = false;
    std::thread shutdownThread(
        [&]
        {
            NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
            shutdownReturned.store(true, std::memory_order_release);
        });

    for (int attempt = 0; attempt < 200 && cleanupStarted.load(std::memory_order_acquire) == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(cleanupStarted.load(std::memory_order_acquire), 1);

    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(handle));
    EXPECT_FALSE(shutdownReturned.load(std::memory_order_acquire));
    EXPECT_EQ(cleanupFinished.load(std::memory_order_acquire), 0);

    releaseCleanup.store(true, std::memory_order_release);
    shutdownThread.join();

    EXPECT_TRUE(shutdownReturned.load(std::memory_order_acquire));
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
    EXPECT_EQ(cleanupFinished.load(std::memory_order_acquire), 1);
    EXPECT_EQ(dependencyCounter.load(std::memory_order_acquire), 0);
}

TEST_F(JobSystemBackgroundTests, ForegroundCallbackRejectsTransitiveBackgroundSelfWait)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS::Base::Jobs::JobHandle background;
    std::atomic<bool> backgroundReady = false;
    std::atomic<bool> foregroundReturned = false;
    CompleteBackgroundFromForegroundData completeData{&background, &backgroundReady, &foregroundReturned};

    NLS::Base::Jobs::JobScheduleDesc foregroundDesc;
    foregroundDesc.function = CompleteBackgroundFromForeground;
    foregroundDesc.userData = &completeData;
    auto foreground = NLS::Base::Jobs::ScheduleJob(foregroundDesc);

    std::atomic<int> foregroundDependencyCounter = 0;
    AtomicCounterData foregroundDependencyData{&foregroundDependencyCounter};
    NLS::Base::Jobs::JobScheduleDesc foregroundDependencyDesc;
    foregroundDependencyDesc.function = IncrementCounter;
    foregroundDependencyDesc.userData = &foregroundDependencyData;
    foregroundDependencyDesc.dependency = foreground;
    auto foregroundDependency = NLS::Base::Jobs::ScheduleJob(foregroundDependencyDesc);

    std::atomic<int> backgroundCounter = 0;
    AtomicCounterData backgroundData{&backgroundCounter};
    NLS::Base::Jobs::BackgroundJobDesc backgroundDesc;
    backgroundDesc.function = IncrementCounter;
    backgroundDesc.userData = &backgroundData;
    backgroundDesc.dependency = foregroundDependency;
    background = NLS::Base::Jobs::ScheduleBackgroundJob(backgroundDesc);
    ASSERT_NE(background.id, 0u);

    backgroundReady.store(true, std::memory_order_release);
    std::thread runner([] { (void)NLS::Base::Jobs::ExecuteOneJobQueueJob(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (!foregroundReturned.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();

    const bool returnedBeforeShutdown = foregroundReturned.load(std::memory_order_acquire);
    if (!returnedBeforeShutdown)
        NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
    runner.join();

    EXPECT_TRUE(returnedBeforeShutdown);
    EXPECT_EQ(foregroundDependencyCounter.load(std::memory_order_acquire), 0);
    EXPECT_EQ(backgroundCounter.load(std::memory_order_acquire), 0);
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

TEST_F(JobSystemBackgroundTests, MainThreadContinuationRunsOnlyWhenDrained)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::vector<int> events;
    ContinuationData data{&events, 7};

    NLS::Base::Jobs::MainThreadContinuationDesc desc;
    desc.function = RecordContinuation;
    desc.userData = &data;

    ASSERT_TRUE(NLS::Base::Jobs::ScheduleMainThreadContinuation(desc));
    EXPECT_TRUE(events.empty());

    EXPECT_EQ(NLS::Base::Jobs::DrainMainThreadContinuations(), 1u);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0], 7);
}

TEST_F(JobSystemBackgroundTests, ContinuationWaitsForDependencyAndPreservesReadyOrder)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    AtomicCounterData jobData{&counter};
    NLS::Base::Jobs::JobScheduleDesc jobDesc;
    jobDesc.function = IncrementCounter;
    jobDesc.userData = &jobData;
    auto dependency = NLS::Base::Jobs::ScheduleJob(jobDesc);

    std::vector<int> events;
    ContinuationData dependent{&events, 2};
    ContinuationData ready{&events, 1};

    NLS::Base::Jobs::MainThreadContinuationDesc dependentDesc;
    dependentDesc.function = RecordContinuation;
    dependentDesc.userData = &dependent;
    dependentDesc.dependency = dependency;
    ASSERT_TRUE(NLS::Base::Jobs::ScheduleMainThreadContinuation(dependentDesc));

    NLS::Base::Jobs::MainThreadContinuationDesc readyDesc;
    readyDesc.function = RecordContinuation;
    readyDesc.userData = &ready;
    ASSERT_TRUE(NLS::Base::Jobs::ScheduleMainThreadContinuation(readyDesc));

    EXPECT_EQ(NLS::Base::Jobs::DrainMainThreadContinuations(), 1u);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0], 1);

    NLS::Base::Jobs::Complete(dependency);
    EXPECT_EQ(NLS::Base::Jobs::DrainMainThreadContinuations(), 1u);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1], 2);
}

TEST_F(JobSystemBackgroundTests, ContinuationWithFailedForegroundDependencyDoesNotBlockLaterReadyContinuation)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS::Base::Jobs::JobScheduleDesc failedDesc;
    failedDesc.function = ThrowingJob;
    auto failedDependency = NLS::Base::Jobs::ScheduleJob(failedDesc);
    ASSERT_NE(failedDependency.id, 0u);
    NLS::Base::Jobs::CompleteNoClear(failedDependency);

    std::vector<int> events;
    ContinuationData blocked{&events, 9};
    ContinuationData ready{&events, 4};

    NLS::Base::Jobs::MainThreadContinuationDesc blockedDesc;
    blockedDesc.function = RecordContinuation;
    blockedDesc.userData = &blocked;
    blockedDesc.dependency = failedDependency;
    ASSERT_TRUE(NLS::Base::Jobs::ScheduleMainThreadContinuation(blockedDesc));

    NLS::Base::Jobs::MainThreadContinuationDesc readyDesc;
    readyDesc.function = RecordContinuation;
    readyDesc.userData = &ready;
    ASSERT_TRUE(NLS::Base::Jobs::ScheduleMainThreadContinuation(readyDesc));

    EXPECT_EQ(NLS::Base::Jobs::DrainMainThreadContinuations(), 1u);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0], 4);
}

TEST_F(JobSystemBackgroundTests, FailedForegroundDependencyContinuationIsRetiredAndReleasesRetainedStatus)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS::Base::Jobs::JobScheduleDesc failedDesc;
    failedDesc.function = ThrowingJob;
    auto failedDependency = NLS::Base::Jobs::ScheduleJob(failedDesc);
    ASSERT_NE(failedDependency.id, 0u);
    NLS::Base::Jobs::CompleteNoClear(failedDependency);

    std::vector<int> events;
    ContinuationData blocked{&events, 9};
    NLS::Base::Jobs::MainThreadContinuationDesc blockedDesc;
    blockedDesc.function = RecordContinuation;
    blockedDesc.userData = &blocked;
    blockedDesc.dependency = failedDependency;
    ASSERT_TRUE(NLS::Base::Jobs::ScheduleMainThreadContinuation(blockedDesc));

    std::atomic<int> churnCounter = 0;
    AtomicCounterData churnData{&churnCounter};
    NLS::Base::Jobs::JobScheduleDesc churnDesc;
    churnDesc.function = IncrementCounter;
    churnDesc.userData = &churnData;
    for (int index = 0; index < 5000; ++index)
    {
        auto churn = NLS::Base::Jobs::ScheduleJob(churnDesc);
        NLS::Base::Jobs::Complete(churn);
    }

    ASSERT_EQ(
        NLS::Base::Jobs::Internal::GetJobCompletionStatus(failedDependency),
        NLS::Base::Jobs::JobCompletionStatus::Failed);
    EXPECT_EQ(NLS::Base::Jobs::DrainMainThreadContinuations(), 0u);
    EXPECT_TRUE(events.empty());
    EXPECT_EQ(
        NLS::Base::Jobs::Internal::GetJobCompletionStatus(failedDependency),
        NLS::Base::Jobs::JobCompletionStatus::Unknown);
}

TEST_F(JobSystemBackgroundTests, ContinuationRetainsForegroundDependencyStatusAfterRetiredHistoryPrune)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> dependencyCounter = 0;
    AtomicCounterData dependencyData{&dependencyCounter};
    NLS::Base::Jobs::JobScheduleDesc dependencyDesc;
    dependencyDesc.function = IncrementCounter;
    dependencyDesc.userData = &dependencyData;
    auto dependency = NLS::Base::Jobs::ScheduleJob(dependencyDesc);
    ASSERT_NE(dependency.id, 0u);

    std::vector<int> events;
    ContinuationData continuationData{&events, 11};
    NLS::Base::Jobs::MainThreadContinuationDesc continuationDesc;
    continuationDesc.function = RecordContinuation;
    continuationDesc.userData = &continuationData;
    continuationDesc.dependency = dependency;
    ASSERT_TRUE(NLS::Base::Jobs::ScheduleMainThreadContinuation(continuationDesc));

    NLS::Base::Jobs::CompleteNoClear(dependency);
    ASSERT_TRUE(NLS::Base::Jobs::IsCompleted(dependency));

    std::atomic<int> churnCounter = 0;
    AtomicCounterData churnData{&churnCounter};
    NLS::Base::Jobs::JobScheduleDesc churnDesc;
    churnDesc.function = IncrementCounter;
    churnDesc.userData = &churnData;
    for (int index = 0; index < 5000; ++index)
    {
        auto churn = NLS::Base::Jobs::ScheduleJob(churnDesc);
        NLS::Base::Jobs::Complete(churn);
    }

    EXPECT_EQ(NLS::Base::Jobs::DrainMainThreadContinuations(), 1u);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0], 11);
}

TEST_F(JobSystemBackgroundTests, ShutdownDrainWaitsForAcceptedBackgroundJobs)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> started = 0;
    std::atomic<int> finished = 0;
    std::atomic<bool> release = false;
    BlockingJobData data{&started, &finished, &release};

    NLS::Base::Jobs::BackgroundJobDesc desc;
    desc.function = BlockingBackgroundJob;
    desc.userData = &data;
    auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);

    for (int attempt = 0; attempt < 200 && started.load(std::memory_order_acquire) == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    ASSERT_EQ(started.load(std::memory_order_acquire), 1);

    std::thread shutdownThread(
        []
        {
            NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::DrainAcceptedWork);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(finished.load(std::memory_order_acquire), 0);

    release.store(true, std::memory_order_release);
    shutdownThread.join();

    EXPECT_EQ(finished.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(handle));
}

TEST_F(JobSystemBackgroundTests, ShutdownImmediateRunsCancelForQueuedBackgroundJobs)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    AtomicCounterData data{&counter};
    NLS::Base::Jobs::JobScheduleDesc dependencyDesc;
    dependencyDesc.function = IncrementCounter;
    dependencyDesc.userData = &data;
    const auto dependency = NLS::Base::Jobs::ScheduleJob(dependencyDesc);

    std::atomic<int> cancelled = 0;
    NLS::Base::Jobs::BackgroundJobDesc desc;
    desc.function = IncrementCounter;
    desc.userData = &data;
    desc.cancelFunction = CountCancel;
    desc.cancelUserData = &cancelled;
    desc.dependency = dependency;
    const auto background = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
    ASSERT_NE(background.id, 0u);

    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);

    EXPECT_EQ(counter.load(std::memory_order_acquire), 0);
    EXPECT_EQ(cancelled.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(background));
}

TEST_F(JobSystemBackgroundTests, ImmediateShutdownReleasesRetainedBackgroundDependencyForCancelledForegroundJob)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> backgroundCounter = 0;
    AtomicCounterData backgroundData{&backgroundCounter};
    NLS::Base::Jobs::BackgroundJobDesc backgroundDesc;
    backgroundDesc.function = IncrementCounter;
    backgroundDesc.userData = &backgroundData;
    const auto background = NLS::Base::Jobs::ScheduleBackgroundJob(backgroundDesc);
    ASSERT_NE(background.id, 0u);
    NLS::Base::Jobs::CompleteNoClear(background);
    ASSERT_EQ(backgroundCounter.load(std::memory_order_acquire), 1);

    std::atomic<int> foregroundCounter = 0;
    AtomicCounterData foregroundData{&foregroundCounter};
    NLS::Base::Jobs::JobScheduleDesc foregroundDesc;
    foregroundDesc.function = IncrementCounter;
    foregroundDesc.userData = &foregroundData;
    foregroundDesc.dependency = background;
    const auto foreground = NLS::Base::Jobs::ScheduleJob(foregroundDesc);
    ASSERT_NE(foreground.id, 0u);

    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);

    EXPECT_EQ(foregroundCounter.load(std::memory_order_acquire), 0);
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));
    EXPECT_EQ(
        NLS::Base::Jobs::Internal::GetJobCompletionStatus(background),
        NLS::Base::Jobs::JobCompletionStatus::Unknown);
}

TEST_F(JobSystemBackgroundTests, ImmediateShutdownDoesNotReportRunningBackgroundJobCompleteBeforeItReturns)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> started = 0;
    std::atomic<int> finished = 0;
    std::atomic<bool> release = false;
    BlockingJobData data{&started, &finished, &release};

    NLS::Base::Jobs::BackgroundJobDesc desc;
    desc.function = BlockingBackgroundJob;
    desc.userData = &data;
    const auto background = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
    ASSERT_NE(background.id, 0u);

    for (int attempt = 0; attempt < 200 && started.load(std::memory_order_acquire) == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(started.load(std::memory_order_acquire), 1);

    std::thread shutdownThread(
        []
        {
            NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(background));

    release.store(true, std::memory_order_release);
    shutdownThread.join();

    EXPECT_EQ(finished.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(background));
}

TEST_F(JobSystemBackgroundTests, CompletingBackgroundJobRunsZeroWorkerShortDependency)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> shortCounter = 0;
    AtomicCounterData shortData{&shortCounter};
    NLS::Base::Jobs::JobScheduleDesc shortDesc;
    shortDesc.function = IncrementCounter;
    shortDesc.userData = &shortData;
    auto dependency = NLS::Base::Jobs::ScheduleJob(shortDesc);

    std::atomic<int> backgroundCounter = 0;
    AtomicCounterData backgroundData{&backgroundCounter};
    NLS::Base::Jobs::BackgroundJobDesc backgroundDesc;
    backgroundDesc.function = IncrementCounter;
    backgroundDesc.userData = &backgroundData;
    backgroundDesc.dependency = dependency;
    auto background = NLS::Base::Jobs::ScheduleBackgroundJob(backgroundDesc);

    NLS::Base::Jobs::Complete(background);

    EXPECT_EQ(shortCounter.load(std::memory_order_acquire), 1);
    EXPECT_EQ(backgroundCounter.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(dependency));
}

TEST_F(JobSystemBackgroundTests, ShutdownDrainPreservesForegroundDependencyForBackgroundJob)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 1u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<bool> release = false;
    std::atomic<bool> flag = false;
    ReleaseFlagData foregroundData{&release, &flag};
    NLS::Base::Jobs::JobScheduleDesc foregroundDesc;
    foregroundDesc.function = SetFlagAfterReleaseJob;
    foregroundDesc.userData = &foregroundData;
    const auto foreground = NLS::Base::Jobs::ScheduleJob(foregroundDesc);
    ASSERT_NE(foreground.id, 0u);

    std::atomic<bool> observed = false;
    FlagJobData backgroundData{&flag, &observed};
    NLS::Base::Jobs::BackgroundJobDesc backgroundDesc;
    backgroundDesc.function = ObserveFlag;
    backgroundDesc.userData = &backgroundData;
    backgroundDesc.dependency = foreground;
    const auto background = NLS::Base::Jobs::ScheduleBackgroundJob(backgroundDesc);
    ASSERT_NE(background.id, 0u);

    std::thread shutdownThread(
        []
        {
            NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::DrainAcceptedWork);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(observed.load(std::memory_order_acquire));

    release.store(true, std::memory_order_release);
    shutdownThread.join();

    EXPECT_TRUE(flag.load(std::memory_order_acquire));
    EXPECT_TRUE(observed.load(std::memory_order_acquire));
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(foreground));
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(background));
}

TEST_F(JobSystemBackgroundTests, ShutdownDrainKicksPendingForegroundDependencyForBackgroundJob)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> foregroundCounter = 0;
    AtomicCounterData foregroundData{&foregroundCounter};
    NLS::Base::Jobs::JobScheduleDesc foregroundDesc;
    foregroundDesc.function = IncrementCounter;
    foregroundDesc.userData = &foregroundData;

    NLS::Base::Jobs::JobBatchDispatcher dispatcher;
    const auto foreground = dispatcher.AddJob(foregroundDesc);
    ASSERT_NE(foreground.id, 0u);

    std::atomic<int> backgroundCounter = 0;
    AtomicCounterData backgroundData{&backgroundCounter};
    NLS::Base::Jobs::BackgroundJobDesc backgroundDesc;
    backgroundDesc.function = IncrementCounter;
    backgroundDesc.userData = &backgroundData;
    backgroundDesc.dependency = foreground;
    const auto background = NLS::Base::Jobs::ScheduleBackgroundJob(backgroundDesc);
    ASSERT_NE(background.id, 0u);

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
    EXPECT_EQ(foregroundCounter.load(std::memory_order_acquire), 1);
    EXPECT_EQ(backgroundCounter.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(foreground));
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(background));
}

TEST_F(JobSystemBackgroundTests, ForegroundJobWaitsForBackgroundDependency)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> started = 0;
    std::atomic<int> finished = 0;
    std::atomic<bool> release = false;
    BlockingJobData backgroundData{&started, &finished, &release};
    NLS::Base::Jobs::BackgroundJobDesc backgroundDesc;
    backgroundDesc.function = SetFlagAfterRelease;
    backgroundDesc.userData = &backgroundData;
    const auto background = NLS::Base::Jobs::ScheduleBackgroundJob(backgroundDesc);

    for (int attempt = 0; attempt < 200 && started.load(std::memory_order_acquire) == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(started.load(std::memory_order_acquire), 1);

    std::atomic<bool> observed = false;
    FlagJobData foregroundData{&release, &observed};
    NLS::Base::Jobs::JobScheduleDesc foregroundDesc;
    foregroundDesc.function = ObserveFlag;
    foregroundDesc.userData = &foregroundData;
    foregroundDesc.dependency = background;
    auto foreground = NLS::Base::Jobs::ScheduleJob(foregroundDesc);

    EXPECT_FALSE(NLS::Base::Jobs::ExecuteOneJobQueueJob());
    EXPECT_FALSE(observed.load(std::memory_order_acquire));

    release.store(true, std::memory_order_release);
    NLS::Base::Jobs::Complete(foreground);

    EXPECT_EQ(finished.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(observed.load(std::memory_order_acquire));
}

TEST_F(JobSystemBackgroundTests, ForegroundFanInWaitsForBackgroundDependencyAfterLocalDependency)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 1u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> localCounter = 0;
    AtomicCounterData localData{&localCounter};
    NLS::Base::Jobs::JobScheduleDesc localDesc;
    localDesc.function = IncrementCounter;
    localDesc.userData = &localData;
    const auto local = NLS::Base::Jobs::ScheduleJob(localDesc);
    ASSERT_NE(local.id, 0u);

    std::atomic<int> finished = 0;
    AtomicCounterData backgroundData{&finished};
    NLS::Base::Jobs::BackgroundJobDesc backgroundDesc;
    backgroundDesc.function = IncrementCounter;
    backgroundDesc.userData = &backgroundData;
    const auto background = NLS::Base::Jobs::ScheduleBackgroundJob(backgroundDesc);
    ASSERT_NE(background.id, 0u);

    const NLS::Base::Jobs::JobHandle dependencies[] = {local, background};
    NLS::Base::Jobs::JobBatchDispatcher dispatcher;
    const auto fanIn = NLS::Base::Jobs::ScheduleMultiDependencyJob(
        dispatcher,
        dependencies,
        std::size(dependencies));
    ASSERT_NE(fanIn.id, 0u);

    NLS::Base::Jobs::CompleteNoClear(fanIn);

    EXPECT_EQ(localCounter.load(std::memory_order_acquire), 1);
    EXPECT_EQ(finished.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(fanIn));
}

TEST_F(JobSystemBackgroundTests, ShutdownDrainPreservesBackgroundDependencyForForegroundJob)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<bool> release = false;
    std::atomic<int> started = 0;
    std::atomic<int> finished = 0;
    BlockingJobData backgroundData{&started, &finished, &release};
    NLS::Base::Jobs::BackgroundJobDesc backgroundDesc;
    backgroundDesc.function = BlockingBackgroundJob;
    backgroundDesc.userData = &backgroundData;
    const auto background = NLS::Base::Jobs::ScheduleBackgroundJob(backgroundDesc);
    ASSERT_NE(background.id, 0u);

    for (int attempt = 0; attempt < 200 && started.load(std::memory_order_acquire) == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(started.load(std::memory_order_acquire), 1);

    std::atomic<int> foregroundCounter = 0;
    AtomicCounterData foregroundData{&foregroundCounter};
    NLS::Base::Jobs::JobScheduleDesc foregroundDesc;
    foregroundDesc.function = IncrementCounter;
    foregroundDesc.userData = &foregroundData;
    foregroundDesc.dependency = background;
    const auto foreground = NLS::Base::Jobs::ScheduleJob(foregroundDesc);
    ASSERT_NE(foreground.id, 0u);

    std::thread shutdownThread(
        []
        {
            NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::DrainAcceptedWork);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(foregroundCounter.load(std::memory_order_acquire), 0);

    release.store(true, std::memory_order_release);
    shutdownThread.join();

    EXPECT_EQ(finished.load(std::memory_order_acquire), 1);
    EXPECT_EQ(foregroundCounter.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(background));
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(foreground));
}

TEST_F(JobSystemBackgroundTests, ShutdownDrainDoesNotRunMainThreadContinuations)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::vector<int> events;
    ContinuationData data{&events, 17};
    NLS::Base::Jobs::MainThreadContinuationDesc desc;
    desc.function = RecordContinuation;
    desc.userData = &data;
    ASSERT_TRUE(NLS::Base::Jobs::ScheduleMainThreadContinuation(desc));

    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::DrainAcceptedWork);

    EXPECT_TRUE(events.empty());
}

TEST_F(JobSystemBackgroundTests, ForegroundCompleteHelpsForegroundDependencyOfBackgroundDependency)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> dependencyCounter = 0;
    AtomicCounterData dependencyData{&dependencyCounter};
    NLS::Base::Jobs::JobScheduleDesc dependencyDesc;
    dependencyDesc.function = IncrementCounter;
    dependencyDesc.userData = &dependencyData;
    auto foregroundDependency = NLS::Base::Jobs::ScheduleJob(dependencyDesc);
    ASSERT_NE(foregroundDependency.id, 0u);

    std::atomic<int> backgroundCounter = 0;
    AtomicCounterData backgroundData{&backgroundCounter};
    NLS::Base::Jobs::BackgroundJobDesc backgroundDesc;
    backgroundDesc.function = IncrementCounter;
    backgroundDesc.userData = &backgroundData;
    backgroundDesc.dependency = foregroundDependency;
    auto background = NLS::Base::Jobs::ScheduleBackgroundJob(backgroundDesc);
    ASSERT_NE(background.id, 0u);

    std::atomic<int> targetCounter = 0;
    AtomicCounterData targetData{&targetCounter};
    NLS::Base::Jobs::JobScheduleDesc targetDesc;
    targetDesc.function = IncrementCounter;
    targetDesc.userData = &targetData;
    targetDesc.dependency = background;
    auto target = NLS::Base::Jobs::ScheduleJob(targetDesc);
    ASSERT_NE(target.id, 0u);

    std::atomic<bool> completeReturned = false;
    std::thread waiter(
        [&]
        {
            NLS::Base::Jobs::CompleteNoClear(target);
            completeReturned.store(true, std::memory_order_release);
        });

    for (int attempt = 0; attempt < 200 && !completeReturned.load(std::memory_order_acquire); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    const bool returnedWithoutExternalHelp = completeReturned.load(std::memory_order_acquire);
    if (!returnedWithoutExternalHelp)
    {
        while (!completeReturned.load(std::memory_order_acquire))
        {
            (void)NLS::Base::Jobs::ExecuteOneJobQueueJob();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    waiter.join();

    EXPECT_TRUE(returnedWithoutExternalHelp);
    EXPECT_EQ(dependencyCounter.load(std::memory_order_acquire), 1);
    EXPECT_EQ(backgroundCounter.load(std::memory_order_acquire), 1);
    EXPECT_EQ(targetCounter.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemBackgroundTests, BackgroundCompleteHelpsAllForegroundFanInDependencies)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> firstCounter = 0;
    std::atomic<int> secondCounter = 0;
    AtomicCounterData firstData{&firstCounter};
    AtomicCounterData secondData{&secondCounter};
    NLS::Base::Jobs::JobScheduleDesc firstDesc;
    firstDesc.function = IncrementCounter;
    firstDesc.userData = &firstData;
    auto first = NLS::Base::Jobs::ScheduleJob(firstDesc);
    NLS::Base::Jobs::JobScheduleDesc secondDesc;
    secondDesc.function = IncrementCounter;
    secondDesc.userData = &secondData;
    auto second = NLS::Base::Jobs::ScheduleJob(secondDesc);

    NLS::Base::Jobs::JobBatchDispatcher dispatcher;
    const NLS::Base::Jobs::JobHandle dependencies[] = {first, second};
    auto fanIn = NLS::Base::Jobs::ScheduleMultiDependencyJob(dispatcher, dependencies, std::size(dependencies));
    ASSERT_NE(fanIn.id, 0u);

    std::atomic<int> backgroundCounter = 0;
    AtomicCounterData backgroundData{&backgroundCounter};
    NLS::Base::Jobs::BackgroundJobDesc backgroundDesc;
    backgroundDesc.function = IncrementCounter;
    backgroundDesc.userData = &backgroundData;
    backgroundDesc.dependency = fanIn;
    auto background = NLS::Base::Jobs::ScheduleBackgroundJob(backgroundDesc);
    ASSERT_NE(background.id, 0u);

    NLS::Base::Jobs::Complete(background);

    EXPECT_EQ(firstCounter.load(std::memory_order_acquire), 1);
    EXPECT_EQ(secondCounter.load(std::memory_order_acquire), 1);
    EXPECT_EQ(backgroundCounter.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(fanIn));
}

TEST_F(JobSystemBackgroundTests, BackgroundWorkerCountZeroStillRunsBackgroundJobs)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    AtomicCounterData data{&counter};
    NLS::Base::Jobs::BackgroundJobDesc desc;
    desc.function = IncrementCounter;
    desc.userData = &data;
    auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
    ASSERT_NE(handle.id, 0u);

    NLS::Base::Jobs::Complete(handle);

    EXPECT_EQ(counter.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemBackgroundTests, ThrowingCancelAndContinuationCallbacksDoNotEscapeShutdown)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> dependencyCounter = 0;
    AtomicCounterData dependencyData{&dependencyCounter};
    NLS::Base::Jobs::JobScheduleDesc dependencyDesc;
    dependencyDesc.function = IncrementCounter;
    dependencyDesc.userData = &dependencyData;
    auto dependency = NLS::Base::Jobs::ScheduleJob(dependencyDesc);

    std::atomic<int> backgroundCounter = 0;
    AtomicCounterData backgroundData{&backgroundCounter};
    NLS::Base::Jobs::BackgroundJobDesc backgroundDesc;
    backgroundDesc.function = IncrementCounter;
    backgroundDesc.userData = &backgroundData;
    backgroundDesc.cancelFunction = ThrowingCancel;
    backgroundDesc.dependency = dependency;
    auto background = NLS::Base::Jobs::ScheduleBackgroundJob(backgroundDesc);
    ASSERT_NE(background.id, 0u);

    NLS::Base::Jobs::MainThreadContinuationDesc continuationDesc;
    continuationDesc.function = ThrowingContinuation;
    ASSERT_TRUE(NLS::Base::Jobs::ScheduleMainThreadContinuation(continuationDesc));

    EXPECT_NO_THROW(NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate));
    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(background));
}

TEST_F(JobSystemBackgroundTests, ForegroundDependentDoesNotRunAfterBackgroundDependencyFails)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS::Base::Jobs::BackgroundJobDesc backgroundDesc;
    backgroundDesc.function = ThrowingJob;
    auto background = NLS::Base::Jobs::ScheduleBackgroundJob(backgroundDesc);
    ASSERT_NE(background.id, 0u);

    std::atomic<int> foregroundCounter = 0;
    AtomicCounterData foregroundData{&foregroundCounter};
    NLS::Base::Jobs::JobScheduleDesc foregroundDesc;
    foregroundDesc.function = IncrementCounter;
    foregroundDesc.userData = &foregroundData;
    foregroundDesc.dependency = background;
    auto foreground = NLS::Base::Jobs::ScheduleJob(foregroundDesc);
    ASSERT_NE(foreground.id, 0u);

    NLS::Base::Jobs::Complete(foreground);

    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(background));
    EXPECT_EQ(foregroundCounter.load(std::memory_order_acquire), 0);
}

TEST_F(JobSystemBackgroundTests, BackgroundDependentDoesNotRunAfterForegroundDependencyFails)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS::Base::Jobs::JobScheduleDesc foregroundDesc;
    foregroundDesc.function = ThrowingJob;
    auto foreground = NLS::Base::Jobs::ScheduleJob(foregroundDesc);
    ASSERT_NE(foreground.id, 0u);

    std::atomic<int> backgroundCounter = 0;
    AtomicCounterData backgroundData{&backgroundCounter};
    NLS::Base::Jobs::BackgroundJobDesc backgroundDesc;
    backgroundDesc.function = IncrementCounter;
    backgroundDesc.userData = &backgroundData;
    std::atomic<int> cancelled = 0;
    backgroundDesc.cancelFunction = CountCancel;
    backgroundDesc.cancelUserData = &cancelled;
    backgroundDesc.dependency = foreground;
    auto background = NLS::Base::Jobs::ScheduleBackgroundJob(backgroundDesc);
    ASSERT_NE(background.id, 0u);

    NLS::Base::Jobs::Complete(background);

    EXPECT_TRUE(NLS::Base::Jobs::IsCompleted(foreground));
    EXPECT_EQ(backgroundCounter.load(std::memory_order_acquire), 0);
    EXPECT_EQ(cancelled.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemBackgroundTests, BackgroundJobWithAlreadyFailedDependencyCancelsWithoutRunning)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS::Base::Jobs::JobScheduleDesc foregroundDesc;
    foregroundDesc.function = ThrowingJob;
    auto foreground = NLS::Base::Jobs::ScheduleJob(foregroundDesc);
    ASSERT_NE(foreground.id, 0u);
    NLS::Base::Jobs::CompleteNoClear(foreground);

    std::atomic<int> backgroundCounter = 0;
    std::atomic<int> cancelled = 0;
    AtomicCounterData backgroundData{&backgroundCounter};
    NLS::Base::Jobs::BackgroundJobDesc backgroundDesc;
    backgroundDesc.function = IncrementCounter;
    backgroundDesc.userData = &backgroundData;
    backgroundDesc.cancelFunction = CountCancel;
    backgroundDesc.cancelUserData = &cancelled;
    backgroundDesc.dependency = foreground;
    auto background = NLS::Base::Jobs::ScheduleBackgroundJob(backgroundDesc);
    ASSERT_NE(background.id, 0u);

    NLS::Base::Jobs::Complete(background);

    EXPECT_EQ(backgroundCounter.load(std::memory_order_acquire), 0);
    EXPECT_EQ(cancelled.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemBackgroundTests, BackgroundJobRejectsUnknownDependencyHandle)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    AtomicCounterData data{&counter};
    NLS::Base::Jobs::BackgroundJobDesc desc;
    desc.function = IncrementCounter;
    desc.userData = &data;
    desc.dependency = {0x1234u, 77u};

    const auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);

    EXPECT_EQ(handle.id, 0u);
    EXPECT_EQ(counter.load(std::memory_order_acquire), 0);
}

TEST_F(JobSystemBackgroundTests, MainThreadContinuationRejectsUnknownDependencyHandle)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::vector<int> events;
    ContinuationData data{&events, 3};
    NLS::Base::Jobs::MainThreadContinuationDesc desc;
    desc.function = RecordContinuation;
    desc.userData = &data;
    desc.dependency = {0x1234u, 77u};

    EXPECT_FALSE(NLS::Base::Jobs::ScheduleMainThreadContinuation(desc));
    EXPECT_EQ(NLS::Base::Jobs::DrainMainThreadContinuations(), 0u);
    EXPECT_TRUE(events.empty());
}

TEST_F(JobSystemBackgroundTests, StaleHandleFromPreviousInitializationDoesNotAliasNewBackgroundJob)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> oldCounter = 0;
    AtomicCounterData oldData{&oldCounter};
    NLS::Base::Jobs::BackgroundJobDesc oldDesc;
    oldDesc.function = IncrementCounter;
    oldDesc.userData = &oldData;
    const auto staleHandle = NLS::Base::Jobs::ScheduleBackgroundJob(oldDesc);
    ASSERT_NE(staleHandle.id, 0u);

    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);

    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> newCounter = 0;
    AtomicCounterData newData{&newCounter};
    NLS::Base::Jobs::BackgroundJobDesc newDesc;
    newDesc.function = IncrementCounter;
    newDesc.userData = &newData;
    const auto newHandle = NLS::Base::Jobs::ScheduleBackgroundJob(newDesc);
    ASSERT_NE(newHandle.id, 0u);

    EXPECT_NE(staleHandle.generation, newHandle.generation);
    EXPECT_FALSE(NLS::Base::Jobs::Internal::IsKnownJobHandle(staleHandle));
    EXPECT_TRUE(NLS::Base::Jobs::Internal::IsKnownJobHandle(newHandle));

    auto handleToComplete = newHandle;
    NLS::Base::Jobs::Complete(handleToComplete);
    EXPECT_EQ(newCounter.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemBackgroundTests, ForegroundQueueDoesNotQueryBackgroundDependencyWhileLocked)
{
    const auto source = ReadRepoFile("Runtime/Base/Jobs/JobQueue.cpp");
    ASSERT_FALSE(source.empty());

    const auto statusBodyBegin = source.find(
        "JobCompletionStatus JobQueue::GetGroupDependenciesStatusLocked(\n"
        "        const GroupPtr& group,");
    ASSERT_NE(statusBodyBegin, std::string::npos);
    const auto registerLocalDependents = source.find("void JobQueue::RegisterLocalDependentsLocked", statusBodyBegin);
    ASSERT_NE(registerLocalDependents, std::string::npos);
    const auto statusBody = source.substr(statusBodyBegin, registerLocalDependents - statusBodyBegin);

    EXPECT_EQ(statusBody.find("Internal::GetJobCompletionStatus"), std::string::npos);
    EXPECT_NE(statusBody.find("FindResolvedDependencyStatus"), std::string::npos);

    const auto wakeExternalBegin = source.find("void JobQueue::WakeExternalDependencyReadyGroups()");
    ASSERT_NE(wakeExternalBegin, std::string::npos);
    const auto collectExternalBegin = source.find("void JobQueue::CollectExternalDependenciesForGroupLocked", wakeExternalBegin);
    ASSERT_NE(collectExternalBegin, std::string::npos);
    const auto wakeExternalBody = source.substr(wakeExternalBegin, collectExternalBegin - wakeExternalBegin);

    EXPECT_NE(wakeExternalBody.find("Internal::GetJobCompletionStatus"), std::string::npos);
    EXPECT_GT(
        wakeExternalBody.find("Internal::GetJobCompletionStatus"),
        wakeExternalBody.find("if (dependenciesToCheck.empty())"));
    EXPECT_LT(
        wakeExternalBody.find("Internal::GetJobCompletionStatus"),
        wakeExternalBody.find("std::lock_guard lock(m_mutex);", wakeExternalBody.find("Internal::GetJobCompletionStatus")));
}

TEST_F(JobSystemBackgroundTests, BackgroundCallbackDoesNotDeadlockCompletingForegroundJobThatDependsOnIt)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS::Base::Jobs::JobHandle foreground;
    std::atomic<bool> backgroundReturned = false;
    CompleteForegroundFromBackgroundData backgroundData{&foreground, &backgroundReturned};
    NLS::Base::Jobs::BackgroundJobDesc backgroundDesc;
    backgroundDesc.function = CompleteForegroundFromBackground;
    backgroundDesc.userData = &backgroundData;
    const auto background = NLS::Base::Jobs::ScheduleBackgroundJob(backgroundDesc);
    ASSERT_NE(background.id, 0u);

    std::atomic<int> foregroundCounter = 0;
    AtomicCounterData foregroundData{&foregroundCounter};
    NLS::Base::Jobs::JobScheduleDesc foregroundDesc;
    foregroundDesc.function = IncrementCounter;
    foregroundDesc.userData = &foregroundData;
    foregroundDesc.dependency = background;
    foreground = NLS::Base::Jobs::ScheduleJob(foregroundDesc);
    ASSERT_NE(foreground.id, 0u);

    for (int attempt = 0; attempt < 200 && !backgroundReturned.load(std::memory_order_acquire); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    EXPECT_TRUE(backgroundReturned.load(std::memory_order_acquire));
    NLS::Base::Jobs::Complete(foreground);
    EXPECT_EQ(foregroundCounter.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemBackgroundTests, CompletingBackgroundJobDoesNotRunUnrelatedForegroundWork)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> unrelatedCounter = 0;
    AtomicCounterData unrelatedData{&unrelatedCounter};
    NLS::Base::Jobs::JobScheduleDesc unrelatedDesc;
    unrelatedDesc.function = IncrementCounter;
    unrelatedDesc.userData = &unrelatedData;
    auto unrelated = NLS::Base::Jobs::ScheduleJob(unrelatedDesc);
    ASSERT_NE(unrelated.id, 0u);

    std::atomic<int> backgroundCounter = 0;
    AtomicCounterData backgroundData{&backgroundCounter};
    NLS::Base::Jobs::BackgroundJobDesc backgroundDesc;
    backgroundDesc.function = IncrementCounter;
    backgroundDesc.userData = &backgroundData;
    auto background = NLS::Base::Jobs::ScheduleBackgroundJob(backgroundDesc);
    ASSERT_NE(background.id, 0u);

    NLS::Base::Jobs::Complete(background);

    EXPECT_EQ(backgroundCounter.load(std::memory_order_acquire), 1);
    EXPECT_EQ(unrelatedCounter.load(std::memory_order_acquire), 0);

    NLS::Base::Jobs::Complete(unrelated);
    EXPECT_EQ(unrelatedCounter.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemBackgroundTests, CompletingBackgroundJobDoesNotRunUnrelatedGuaranteedNoSyncForegroundWork)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> unrelatedCounter = 0;
    AtomicCounterData unrelatedData{&unrelatedCounter};
    NLS::Base::Jobs::JobScheduleDesc unrelatedDesc;
    unrelatedDesc.function = IncrementCounter;
    unrelatedDesc.userData = &unrelatedData;
    unrelatedDesc.priority = NLS::Base::Jobs::JobPriority::High;
    unrelatedDesc.safetyPolicy = NLS::Base::Jobs::JobSafetyPolicy::GuaranteedNoSyncWait;
    auto unrelated = NLS::Base::Jobs::ScheduleJob(unrelatedDesc);
    ASSERT_NE(unrelated.id, 0u);

    std::atomic<int> dependencyCounter = 0;
    AtomicCounterData dependencyData{&dependencyCounter};
    NLS::Base::Jobs::JobScheduleDesc dependencyDesc;
    dependencyDesc.function = IncrementCounter;
    dependencyDesc.userData = &dependencyData;
    auto dependency = NLS::Base::Jobs::ScheduleJob(dependencyDesc);
    ASSERT_NE(dependency.id, 0u);

    std::atomic<int> backgroundCounter = 0;
    AtomicCounterData backgroundData{&backgroundCounter};
    NLS::Base::Jobs::BackgroundJobDesc backgroundDesc;
    backgroundDesc.function = IncrementCounter;
    backgroundDesc.userData = &backgroundData;
    backgroundDesc.dependency = dependency;
    auto background = NLS::Base::Jobs::ScheduleBackgroundJob(backgroundDesc);
    ASSERT_NE(background.id, 0u);

    NLS::Base::Jobs::Complete(background);

    EXPECT_EQ(dependencyCounter.load(std::memory_order_acquire), 1);
    EXPECT_EQ(backgroundCounter.load(std::memory_order_acquire), 1);
    EXPECT_EQ(unrelatedCounter.load(std::memory_order_acquire), 0);
    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(unrelated));

    NLS::Base::Jobs::Complete(unrelated);
    EXPECT_EQ(unrelatedCounter.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemBackgroundTests, CompletingBackgroundJobDoesNotHelpUnrelatedBackgroundForegroundDependency)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> targetDependencyCounter = 0;
    AtomicCounterData targetDependencyData{&targetDependencyCounter};
    NLS::Base::Jobs::JobScheduleDesc targetDependencyDesc;
    targetDependencyDesc.function = IncrementCounter;
    targetDependencyDesc.userData = &targetDependencyData;
    auto targetDependency = NLS::Base::Jobs::ScheduleJob(targetDependencyDesc);
    ASSERT_NE(targetDependency.id, 0u);

    std::atomic<int> targetBackgroundCounter = 0;
    AtomicCounterData targetBackgroundData{&targetBackgroundCounter};
    NLS::Base::Jobs::BackgroundJobDesc targetBackgroundDesc;
    targetBackgroundDesc.function = IncrementCounter;
    targetBackgroundDesc.userData = &targetBackgroundData;
    targetBackgroundDesc.dependency = targetDependency;
    auto targetBackground = NLS::Base::Jobs::ScheduleBackgroundJob(targetBackgroundDesc);
    ASSERT_NE(targetBackground.id, 0u);

    std::atomic<int> unrelatedDependencyCounter = 0;
    AtomicCounterData unrelatedDependencyData{&unrelatedDependencyCounter};
    NLS::Base::Jobs::JobScheduleDesc unrelatedDependencyDesc;
    unrelatedDependencyDesc.function = IncrementCounter;
    unrelatedDependencyDesc.userData = &unrelatedDependencyData;
    auto unrelatedDependency = NLS::Base::Jobs::ScheduleJob(unrelatedDependencyDesc);
    ASSERT_NE(unrelatedDependency.id, 0u);

    std::atomic<int> unrelatedBackgroundCounter = 0;
    AtomicCounterData unrelatedBackgroundData{&unrelatedBackgroundCounter};
    NLS::Base::Jobs::BackgroundJobDesc unrelatedBackgroundDesc;
    unrelatedBackgroundDesc.function = IncrementCounter;
    unrelatedBackgroundDesc.userData = &unrelatedBackgroundData;
    unrelatedBackgroundDesc.dependency = unrelatedDependency;
    auto unrelatedBackground = NLS::Base::Jobs::ScheduleBackgroundJob(unrelatedBackgroundDesc);
    ASSERT_NE(unrelatedBackground.id, 0u);

    NLS::Base::Jobs::Complete(targetBackground);

    EXPECT_EQ(targetDependencyCounter.load(std::memory_order_acquire), 1);
    EXPECT_EQ(targetBackgroundCounter.load(std::memory_order_acquire), 1);
    EXPECT_EQ(unrelatedDependencyCounter.load(std::memory_order_acquire), 0);
    EXPECT_EQ(unrelatedBackgroundCounter.load(std::memory_order_acquire), 0);
    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(unrelatedBackground));

    NLS::Base::Jobs::CompleteNoClear(unrelatedDependency);
    NLS::Base::Jobs::CompleteNoClear(unrelatedBackground);
}

TEST_F(JobSystemBackgroundTests, CompletingBackgroundJobDoesNotRunUnrelatedBackgroundCancelCleanup)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> blockerStarted = 0;
    std::atomic<int> blockerFinished = 0;
    std::atomic<bool> releaseBlocker = false;
    BlockingJobData blockerData{&blockerStarted, &blockerFinished, &releaseBlocker};
    NLS::Base::Jobs::BackgroundJobDesc blockerDesc;
    blockerDesc.function = BlockingBackgroundJob;
    blockerDesc.userData = &blockerData;
    auto blocker = NLS::Base::Jobs::ScheduleBackgroundJob(blockerDesc);
    ASSERT_NE(blocker.id, 0u);
    for (int attempt = 0; attempt < 200 && blockerStarted.load(std::memory_order_acquire) == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(blockerStarted.load(std::memory_order_acquire), 1);

    std::atomic<int> targetDependencyCounter = 0;
    AtomicCounterData targetDependencyData{&targetDependencyCounter};
    NLS::Base::Jobs::JobScheduleDesc targetDependencyDesc;
    targetDependencyDesc.function = IncrementCounter;
    targetDependencyDesc.userData = &targetDependencyData;
    auto targetDependency = NLS::Base::Jobs::ScheduleJob(targetDependencyDesc);
    ASSERT_NE(targetDependency.id, 0u);

    std::atomic<int> targetCounter = 0;
    AtomicCounterData targetData{&targetCounter};
    NLS::Base::Jobs::BackgroundJobDesc targetDesc;
    targetDesc.function = IncrementCounter;
    targetDesc.userData = &targetData;
    targetDesc.dependency = targetDependency;
    auto targetBackground = NLS::Base::Jobs::ScheduleBackgroundJob(targetDesc);
    ASSERT_NE(targetBackground.id, 0u);

    NLS::Base::Jobs::JobScheduleDesc failingDependencyDesc;
    failingDependencyDesc.function = ThrowingJob;
    auto failingDependency = NLS::Base::Jobs::ScheduleJob(failingDependencyDesc);
    ASSERT_NE(failingDependency.id, 0u);

    std::atomic<int> cancelStarted = 0;
    NLS::Base::Jobs::BackgroundJobDesc unrelatedDesc;
    unrelatedDesc.function = IncrementCounter;
    unrelatedDesc.userData = &targetData;
    unrelatedDesc.cancelFunction = CountCancel;
    unrelatedDesc.cancelUserData = &cancelStarted;
    unrelatedDesc.dependency = failingDependency;
    auto unrelatedBackground = NLS::Base::Jobs::ScheduleBackgroundJob(unrelatedDesc);
    ASSERT_NE(unrelatedBackground.id, 0u);
    NLS::Base::Jobs::CompleteNoClear(failingDependency);

    std::atomic<bool> targetCompleteReturned = false;
    std::thread targetWaiter(
        [&]
        {
            NLS::Base::Jobs::Complete(targetBackground);
            targetCompleteReturned.store(true, std::memory_order_release);
        });

    for (int attempt = 0; attempt < 200 && targetDependencyCounter.load(std::memory_order_acquire) == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_EQ(targetDependencyCounter.load(std::memory_order_acquire), 1);
    EXPECT_EQ(cancelStarted.load(std::memory_order_acquire), 0);

    releaseBlocker.store(true, std::memory_order_release);
    targetWaiter.join();

    EXPECT_EQ(targetCounter.load(std::memory_order_acquire), 1);
    EXPECT_EQ(blockerFinished.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(targetCompleteReturned.load(std::memory_order_acquire));

    NLS::Base::Jobs::CompleteNoClear(unrelatedBackground);
}

TEST_F(JobSystemBackgroundTests, CompletingBackgroundJobDoesNotRunUnrelatedForegroundExternalCancelCleanup)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS::Base::Jobs::BackgroundJobDesc failingBackgroundDesc;
    failingBackgroundDesc.function = ThrowingBackgroundJob;
    auto failingBackground = NLS::Base::Jobs::ScheduleBackgroundJob(failingBackgroundDesc);
    ASSERT_NE(failingBackground.id, 0u);

    std::atomic<int> unrelatedForegroundRuns = 0;
    std::atomic<int> unrelatedForegroundCancelCount = 0;
    AtomicCounterData unrelatedForegroundData{&unrelatedForegroundRuns};
    NLS::Base::Jobs::JobScheduleDesc unrelatedForegroundDesc;
    unrelatedForegroundDesc.function = IncrementCounter;
    unrelatedForegroundDesc.userData = &unrelatedForegroundData;
    unrelatedForegroundDesc.cancelFunction = CountCancel;
    unrelatedForegroundDesc.cancelUserData = &unrelatedForegroundCancelCount;
    unrelatedForegroundDesc.dependency = failingBackground;
    auto unrelatedForeground = NLS::Base::Jobs::ScheduleJob(unrelatedForegroundDesc);
    ASSERT_NE(unrelatedForeground.id, 0u);
    NLS::Base::Jobs::CompleteNoClear(failingBackground);

    std::atomic<int> targetDependencyCounter = 0;
    AtomicCounterData targetDependencyData{&targetDependencyCounter};
    NLS::Base::Jobs::JobScheduleDesc targetDependencyDesc;
    targetDependencyDesc.function = IncrementCounter;
    targetDependencyDesc.userData = &targetDependencyData;
    auto targetDependency = NLS::Base::Jobs::ScheduleJob(targetDependencyDesc);
    ASSERT_NE(targetDependency.id, 0u);

    std::atomic<int> targetCounter = 0;
    AtomicCounterData targetData{&targetCounter};
    NLS::Base::Jobs::BackgroundJobDesc targetBackgroundDesc;
    targetBackgroundDesc.function = IncrementCounter;
    targetBackgroundDesc.userData = &targetData;
    targetBackgroundDesc.dependency = targetDependency;
    auto targetBackground = NLS::Base::Jobs::ScheduleBackgroundJob(targetBackgroundDesc);
    ASSERT_NE(targetBackground.id, 0u);

    NLS::Base::Jobs::Complete(targetBackground);

    EXPECT_EQ(targetDependencyCounter.load(std::memory_order_acquire), 1);
    EXPECT_EQ(targetCounter.load(std::memory_order_acquire), 1);
    EXPECT_EQ(unrelatedForegroundRuns.load(std::memory_order_acquire), 0);
    EXPECT_EQ(unrelatedForegroundCancelCount.load(std::memory_order_acquire), 0);

    NLS::Base::Jobs::CompleteNoClear(unrelatedForeground);
    EXPECT_EQ(unrelatedForegroundCancelCount.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemBackgroundTests, HelpedForegroundCallbackOnBackgroundWorkerRejectsBackgroundWait)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    config.enableDiagnostics = true;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> gateCounter = 0;
    AtomicCounterData gateData{&gateCounter};
    NLS::Base::Jobs::JobScheduleDesc gateDesc;
    gateDesc.function = IncrementCounter;
    gateDesc.userData = &gateData;
    NLS::Base::Jobs::JobBatchDispatcher gateDispatcher;
    auto gate = gateDispatcher.AddJob(gateDesc);
    ASSERT_NE(gate.id, 0u);

    std::atomic<int> blockedCounter = 0;
    AtomicCounterData blockedData{&blockedCounter};
    NLS::Base::Jobs::BackgroundJobDesc blockedDesc;
    blockedDesc.function = IncrementCounter;
    blockedDesc.userData = &blockedData;
    blockedDesc.dependency = gate;
    auto blockedBackground = NLS::Base::Jobs::ScheduleBackgroundJob(blockedDesc);
    ASSERT_NE(blockedBackground.id, 0u);

    std::atomic<bool> foregroundReturned = false;
    CompleteBackgroundFromHelpedForegroundData foregroundData{&blockedBackground, &foregroundReturned};
    NLS::Base::Jobs::JobScheduleDesc foregroundDependencyDesc;
    foregroundDependencyDesc.function = CompleteBackgroundFromHelpedForeground;
    foregroundDependencyDesc.userData = &foregroundData;
    auto foregroundDependency = NLS::Base::Jobs::ScheduleJob(foregroundDependencyDesc);
    ASSERT_NE(foregroundDependency.id, 0u);

    std::atomic<bool> helperReturned = false;
    NLS::Base::Jobs::BackgroundJobDesc helperDesc;
    helperDesc.function = [](void* userData)
    {
        auto* returned = static_cast<std::atomic<bool>*>(userData);
        (void)NLS::Base::Jobs::ExecuteOneJobQueueJob();
        returned->store(true, std::memory_order_release);
    };
    helperDesc.userData = &helperReturned;
    auto helper = NLS::Base::Jobs::ScheduleBackgroundJob(helperDesc);
    ASSERT_NE(helper.id, 0u);

    for (int attempt = 0; attempt < 200 && !foregroundReturned.load(std::memory_order_acquire); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_TRUE(foregroundReturned.load(std::memory_order_acquire));
    EXPECT_TRUE(helperReturned.load(std::memory_order_acquire));

    EXPECT_EQ(gateCounter.load(std::memory_order_acquire), 0);
    EXPECT_EQ(blockedCounter.load(std::memory_order_acquire), 0);
    EXPECT_FALSE(NLS::Base::Jobs::IsCompleted(blockedBackground));

    const auto snapshot = NLS::Base::Jobs::CopyJobDiagnosticSnapshot();
    EXPECT_TRUE(std::any_of(
        snapshot.recentViolations.begin(),
        snapshot.recentViolations.end(),
        [](const NLS::Base::Jobs::JobViolationRecord& record)
        {
            return record.kind == NLS::Base::Jobs::JobViolationKind::SyncWaitDisallowed;
        }));

    NLS::Base::Jobs::CompleteNoClear(helper);
    NLS::Base::Jobs::CompleteNoClear(gate);
    NLS::Base::Jobs::CompleteNoClear(blockedBackground);
}

TEST_F(JobSystemBackgroundTests, ForegroundCallbackDoesNotDeadlockCompletingBackgroundJobThatDependsOnIt)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 1u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS::Base::Jobs::JobHandle background;
    std::atomic<bool> backgroundReady = false;
    std::atomic<bool> foregroundReturned = false;
    CompleteBackgroundFromForegroundData foregroundData{&background, &backgroundReady, &foregroundReturned};
    NLS::Base::Jobs::JobScheduleDesc foregroundDesc;
    foregroundDesc.function = CompleteBackgroundFromForeground;
    foregroundDesc.userData = &foregroundData;
    const auto foreground = NLS::Base::Jobs::ScheduleJob(foregroundDesc);
    ASSERT_NE(foreground.id, 0u);

    std::atomic<int> backgroundCounter = 0;
    AtomicCounterData backgroundData{&backgroundCounter};
    NLS::Base::Jobs::BackgroundJobDesc backgroundDesc;
    backgroundDesc.function = IncrementCounter;
    backgroundDesc.userData = &backgroundData;
    backgroundDesc.dependency = foreground;
    background = NLS::Base::Jobs::ScheduleBackgroundJob(backgroundDesc);
    ASSERT_NE(background.id, 0u);
    backgroundReady.store(true, std::memory_order_release);

    for (int attempt = 0; attempt < 200 && !foregroundReturned.load(std::memory_order_acquire); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    EXPECT_TRUE(foregroundReturned.load(std::memory_order_acquire));
    NLS::Base::Jobs::Complete(background);
    EXPECT_EQ(backgroundCounter.load(std::memory_order_acquire), 1);
}
