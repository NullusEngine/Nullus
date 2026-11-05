#include <gtest/gtest.h>

#include "Jobs/JobSystem.h"
#include "Jobs/JobDiagnostics.h"
#include "Jobs/BackgroundJobQueue.h"
#include "TaskGraph/TaskGraph.h"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    namespace Jobs = NLS::Base::Jobs;
    using namespace std::chrono_literals;

    class JobsUnifiedSchedulerTests : public ::testing::Test
    {
    protected:
        void TearDown() override
        {
            Jobs::ShutdownJobSystem();
#if defined(NLS_ENABLE_TEST_HOOKS)
            Jobs::ResetJobSystemForTesting();
#endif
        }
    };

    Jobs::JobSystemConfig Config(uint32_t workers)
    {
        Jobs::JobSystemConfig config;
        config.workerCount = workers;
        config.backgroundWorkerCount = 1u;
        return config;
    }

    void Increment(void* userData)
    {
        static_cast<std::atomic<size_t>*>(userData)->fetch_add(1u, std::memory_order_relaxed);
    }

    bool WaitUntil(const std::function<bool()>& predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!predicate() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        return predicate();
    }
}

TEST_F(JobsUnifiedSchedulerTests, LeasesShareFirstConfigurationAndLastReleaseDrains)
{
    ASSERT_TRUE(Jobs::Internal::AcquireJobSystem(Config(1u)));
    ASSERT_TRUE(Jobs::Internal::AcquireJobSystem(Config(4u)));
    EXPECT_EQ(Jobs::GetJobWorkerCount(), 1u);
    std::atomic<size_t> completed{0u};
    Jobs::JobScheduleDesc desc;
    desc.function = Increment;
    desc.userData = &completed;
    ASSERT_NE(Jobs::ScheduleJob(desc).id, 0u);
    Jobs::Internal::ReleaseJobSystem();
    EXPECT_TRUE(Jobs::IsJobSystemInitialized());
    Jobs::Internal::ReleaseJobSystem();
    EXPECT_FALSE(Jobs::IsJobSystemInitialized());
    EXPECT_EQ(completed.load(), 1u);
}

TEST_F(JobsUnifiedSchedulerTests, ExplicitOwnerCanClaimPoolCreatedByGraphLease)
{
    ASSERT_TRUE(Jobs::Internal::AcquireJobSystem(Config(1u)));
    EXPECT_TRUE(Jobs::TryInitializeJobSystem(Config(3u)));
    EXPECT_FALSE(Jobs::TryInitializeJobSystem(Config(3u)));
    EXPECT_EQ(Jobs::GetJobWorkerCount(), 1u);
    Jobs::Internal::ReleaseJobSystem();
    EXPECT_TRUE(Jobs::IsJobSystemInitialized());
    Jobs::ShutdownJobSystem();
    EXPECT_FALSE(Jobs::IsJobSystemInitialized());
}

TEST_F(JobsUnifiedSchedulerTests, ExplicitShutdownDefersUntilGraphLeaseReleases)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(1u)));
    ASSERT_TRUE(Jobs::Internal::AcquireJobSystem(Config(1u)));
    Jobs::ShutdownJobSystem();
    EXPECT_TRUE(Jobs::IsJobSystemInitialized());
    std::atomic<size_t> completed{0u};
    Jobs::JobScheduleDesc desc;
    desc.function = Increment;
    desc.userData = &completed;
    ASSERT_NE(Jobs::ScheduleJob(desc).id, 0u);
    Jobs::Internal::ReleaseJobSystem();
    EXPECT_EQ(completed.load(), 1u);
    EXPECT_FALSE(Jobs::IsJobSystemInitialized());
}

TEST_F(JobsUnifiedSchedulerTests, WorkerOnlyHelpDoesNotExecuteJobsOnUnregisteredCaller)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(0u)));
    std::atomic<size_t> completed{0u};
    Jobs::JobScheduleDesc desc;
    desc.function = Increment;
    desc.userData = &completed;
    auto handle = Jobs::ScheduleJob(desc);
    EXPECT_FALSE(Jobs::Internal::IsForegroundWorkerThread());
    EXPECT_FALSE(Jobs::Internal::ExecuteOneForegroundJobForWorker());
    EXPECT_EQ(completed.load(), 0u);
    Jobs::Complete(handle);
    EXPECT_EQ(completed.load(), 1u);
}

TEST_F(JobsUnifiedSchedulerTests, SingleWorkerCanHelpMaySyncWaitChild)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(1u)));
    struct Data
    {
        std::atomic<size_t> children{0u};
        std::atomic<bool> worker{false};
        std::promise<bool> result;
    } data;
    auto result = data.result.get_future();
    Jobs::JobScheduleDesc desc;
    desc.userData = &data;
    desc.function = [](void* raw)
    {
        auto& data = *static_cast<Data*>(raw);
        data.worker.store(Jobs::Internal::IsForegroundWorkerThread());
        Jobs::JobScheduleDesc child;
        child.function = Increment;
        child.userData = &data.children;
        const auto handle = Jobs::ScheduleJob(child);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!Jobs::IsCompleted(handle) && std::chrono::steady_clock::now() < deadline)
        {
            if (!Jobs::Internal::ExecuteOneForegroundJobForWorker())
                std::this_thread::sleep_for(1ms);
        }
        data.result.set_value(Jobs::IsCompleted(handle));
    };
    ASSERT_NE(Jobs::ScheduleJob(desc).id, 0u);
    ASSERT_EQ(result.wait_for(6s), std::future_status::ready);
    EXPECT_TRUE(result.get());
    EXPECT_TRUE(data.worker.load());
    EXPECT_EQ(data.children.load(), 1u);
    EXPECT_GT(Jobs::Internal::GetJobWorkerLocalExecutionCount(), 0u);
}

TEST_F(JobsUnifiedSchedulerTests, OtherWorkersStealLocallySubmittedWork)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(4u)));
    struct Data
    {
        std::atomic<size_t> completed{0u};
        std::atomic<size_t> accepted{0u};
        std::atomic<bool> submitted{false};
        std::atomic<bool> release{false};
        std::promise<void> parentFinished;
    } data;
    auto finished = data.parentFinished.get_future();
    Jobs::JobScheduleDesc parent;
    parent.userData = &data;
    parent.function = [](void* raw)
    {
        auto& data = *static_cast<Data*>(raw);
        Jobs::JobScheduleDesc child;
        child.function = Increment;
        child.userData = &data.completed;
        for (size_t i = 0u; i < 256u; ++i)
            if (Jobs::ScheduleJob(child).id != 0u)
                data.accepted.fetch_add(1u);
        data.submitted.store(true);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!data.release.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        data.parentFinished.set_value();
    };
    ASSERT_NE(Jobs::ScheduleJob(parent).id, 0u);
    const bool submitted = WaitUntil([&] { return data.submitted.load(); });
    const bool stolen = submitted && WaitUntil([&] { return data.completed.load() == 256u; });
    data.release.store(true);
    ASSERT_EQ(finished.wait_for(6s), std::future_status::ready);
    EXPECT_TRUE(submitted);
    EXPECT_TRUE(stolen);
    EXPECT_EQ(data.accepted.load(), 256u);
    EXPECT_GT(Jobs::Internal::GetJobWorkerStealCount(), 0u);
    EXPECT_GT(Jobs::Internal::GetJobWorkerInjectionExecutionCount(), 0u);
}

TEST_F(JobsUnifiedSchedulerTests, CapacityRejectsWithoutConsumingCallerPayloadAndRecovers)
{
    auto config = Config(0u);
    config.maxPendingJobCount = 2u;
    ASSERT_TRUE(Jobs::InitializeJobSystem(config));
    std::atomic<size_t> completed{0u};
    std::atomic<size_t> cancelled{0u};
    Jobs::JobScheduleDesc desc;
    desc.function = Increment;
    desc.userData = &completed;
    desc.cancelFunction = Increment;
    desc.cancelUserData = &cancelled;
    auto first = Jobs::ScheduleJob(desc);
    auto second = Jobs::ScheduleJob(desc);
    ASSERT_NE(first.id, 0u);
    ASSERT_NE(second.id, 0u);
    EXPECT_EQ(Jobs::ScheduleJob(desc).id, 0u);
    EXPECT_EQ(cancelled.load(), 0u);
    Jobs::Complete(first);
    auto third = Jobs::ScheduleJob(desc);
    ASSERT_NE(third.id, 0u);
    Jobs::Complete(second);
    Jobs::Complete(third);
    EXPECT_EQ(completed.load(), 3u);
    EXPECT_EQ(cancelled.load(), 0u);
}

TEST_F(JobsUnifiedSchedulerTests, ThreePrioritiesKeepNormalAndHighAbiValues)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(0u)));
    EXPECT_EQ(static_cast<uint8_t>(Jobs::JobPriority::Normal), 0u);
    EXPECT_EQ(static_cast<uint8_t>(Jobs::JobPriority::High), 1u);
    struct Data { std::vector<int>* order; int value; };
    std::vector<int> order;
    Data low{&order, 0}, normal{&order, 1}, high{&order, 2};
    auto submit = [](Data& data, Jobs::JobPriority priority)
    {
        Jobs::JobScheduleDesc desc;
        desc.userData = &data;
        desc.priority = priority;
        desc.function = [](void* raw)
        {
            auto& data = *static_cast<Data*>(raw);
            data.order->push_back(data.value);
        };
        return Jobs::ScheduleJob(desc);
    };
    ASSERT_NE(submit(low, Jobs::JobPriority::Low).id, 0u);
    ASSERT_NE(submit(normal, Jobs::JobPriority::Normal).id, 0u);
    ASSERT_NE(submit(high, Jobs::JobPriority::High).id, 0u);
    while (Jobs::ExecuteOneJobQueueJob()) {}
    EXPECT_EQ(order, (std::vector<int>{2, 1, 0}));
}

TEST_F(JobsUnifiedSchedulerTests, ConcurrentSubmitAndWaitPreservesCallbackLifetime)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(4u)));
    std::atomic<size_t> completed{0u};
    std::atomic<size_t> rejected{0u};
    std::vector<std::thread> submitters;
    for (size_t thread = 0u; thread < 8u; ++thread)
    {
        submitters.emplace_back([&]
        {
            for (size_t i = 0u; i < 128u; ++i)
            {
                Jobs::JobScheduleDesc desc;
                desc.function = Increment;
                desc.userData = &completed;
                auto handle = Jobs::ScheduleJob(desc);
                if (handle.id == 0u)
                    rejected.fetch_add(1u);
                Jobs::Complete(handle);
            }
        });
    }
    for (auto& thread : submitters)
        thread.join();
    EXPECT_EQ(rejected.load(), 0u);
    EXPECT_EQ(completed.load(), 1024u);
}

TEST_F(JobsUnifiedSchedulerTests, DrainBarrierPumpsOwnerAndPreservesScheduler)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(1u)));
    struct Data
    {
        std::atomic<bool> pumped{false};
        std::atomic<bool> workerEntered{false};
        std::atomic<bool> finished{false};
        std::atomic<size_t> continuationCount{0u};
    } data;
    Jobs::JobScheduleDesc desc;
    desc.userData = &data;
    desc.function = [](void* raw)
    {
        auto& data = *static_cast<Data*>(raw);
        data.workerEntered.store(true);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!data.pumped.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        Jobs::MainThreadContinuationDesc continuation;
        continuation.function = Increment;
        continuation.userData = &data.continuationCount;
        Jobs::ScheduleMainThreadContinuation(continuation);
        data.finished.store(data.pumped.load());
    };
    ASSERT_NE(Jobs::ScheduleJob(desc).id, 0u);
    const bool entered = WaitUntil([&] { return data.workerEntered.load(); });
    Jobs::DrainAcceptedJobs([&] { data.pumped.store(true); });
    EXPECT_TRUE(entered);
    EXPECT_TRUE(data.finished.load());
    EXPECT_EQ(data.continuationCount.load(), 1u);
    EXPECT_TRUE(Jobs::IsJobSystemInitialized());
    Jobs::DrainAcceptedJobs();
}


TEST_F(JobsUnifiedSchedulerTests, QueuedCancellationReleasesPayloadAndCancelsDependents)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(0u)));
    std::atomic<size_t> executed{0u};
    std::atomic<size_t> cancelled{0u};
    Jobs::JobScheduleDesc desc;
    desc.function = Increment;
    desc.userData = &executed;
    desc.cancelFunction = Increment;
    desc.cancelUserData = &cancelled;
    const auto root = Jobs::ScheduleJob(desc);
    const auto dependent = Jobs::ScheduleJobDepends(desc, root);
    ASSERT_NE(root.id, 0u);
    ASSERT_NE(dependent.id, 0u);
    EXPECT_TRUE(Jobs::RequestCancel(root));
    Jobs::CompleteNoClear(dependent);
    EXPECT_EQ(Jobs::GetCompletionStatus(root), Jobs::JobCompletionStatus::Cancelled);
    EXPECT_EQ(Jobs::GetCompletionStatus(dependent), Jobs::JobCompletionStatus::Cancelled);
    EXPECT_TRUE(Jobs::IsCancellationRequested(root));
    EXPECT_FALSE(Jobs::RequestCancel(root));
    EXPECT_EQ(executed.load(), 0u);
    EXPECT_EQ(cancelled.load(), 2u);
}

TEST_F(JobsUnifiedSchedulerTests, RunningCancellationIsCooperativeAndDoesNotDestroyPayload)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(1u)));
    struct Data
    {
        std::atomic<bool> entered{false};
        std::atomic<bool> release{false};
        std::atomic<bool> observed{false};
        std::atomic<size_t> cleanup{0u};
        Jobs::JobHandle handle;
    } data;
    Jobs::JobScheduleDesc desc;
    desc.userData = &data;
    desc.function = [](void* raw)
    {
        auto& data = *static_cast<Data*>(raw);
        data.entered.store(true);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!data.release.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        if (data.release.load())
            data.observed.store(Jobs::IsCancellationRequested(data.handle));
    };
    desc.cancelFunction = Increment;
    desc.cancelUserData = &data.cleanup;
    data.handle = Jobs::ScheduleJob(desc);
    const bool entered = WaitUntil([&] { return data.entered.load(); });
    EXPECT_TRUE(Jobs::RequestCancel(data.handle));
    data.release.store(true);
    Jobs::CompleteNoClear(data.handle);
    EXPECT_TRUE(entered);
    EXPECT_TRUE(data.observed.load());
    EXPECT_EQ(Jobs::GetCompletionStatus(data.handle), Jobs::JobCompletionStatus::Succeeded);
    EXPECT_EQ(data.cleanup.load(), 0u);
}

TEST_F(JobsUnifiedSchedulerTests, DrainIncludesFireAndForgetDescendants)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(0u)));
    std::atomic<size_t> completed{0u};
    Jobs::JobScheduleDesc parent;
    parent.userData = &completed;
    parent.function = [](void* raw)
    {
        Jobs::JobScheduleDesc child;
        child.function = Increment;
        child.userData = raw;
        Jobs::ScheduleJob(child);
    };
    ASSERT_NE(Jobs::ScheduleJob(parent).id, 0u);
    Jobs::DrainAcceptedJobs();
    EXPECT_EQ(completed.load(), 1u);
}

TEST_F(JobsUnifiedSchedulerTests, DrainIncludesJobsPublishedByBackgroundMainContinuation)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(0u)));
    std::atomic<size_t> completed{0u};
    Jobs::BackgroundJobDesc background;
    background.userData = &completed;
    background.function = [](void* raw)
    {
        Jobs::MainThreadContinuationDesc continuation;
        continuation.userData = raw;
        continuation.function = [](void* inner)
        {
            Jobs::JobScheduleDesc child;
            child.function = Increment;
            child.userData = inner;
            Jobs::ScheduleJob(child);
        };
        Jobs::ScheduleMainThreadContinuation(continuation);
    };
    ASSERT_NE(Jobs::ScheduleBackgroundJob(background).id, 0u);
    Jobs::DrainAcceptedJobs();
    EXPECT_EQ(completed.load(), 1u);
}


TEST_F(JobsUnifiedSchedulerTests, MainContinuationsRejectForeignThreadDraining)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(0u)));
    std::atomic<size_t> completed{0u};
    Jobs::MainThreadContinuationDesc desc;
    desc.function = Increment;
    desc.userData = &completed;
    ASSERT_TRUE(Jobs::ScheduleMainThreadContinuation(desc));
    uint32_t foreignCount = 99u;
    std::thread foreign([&] { foreignCount = Jobs::DrainMainThreadContinuations(); });
    foreign.join();
    EXPECT_EQ(foreignCount, 0u);
    EXPECT_EQ(completed.load(), 0u);
    EXPECT_EQ(Jobs::DrainMainThreadContinuations(), 1u);
    EXPECT_EQ(completed.load(), 1u);
}

TEST_F(JobsUnifiedSchedulerTests, ExplicitOwnerClaimsContinuationAffinityFromLeaseOnlyStartup)
{
    std::atomic<bool> acquired{false};
    std::atomic<size_t> completed{0u};
    std::thread starter([&]
    {
        acquired.store(Jobs::Internal::AcquireJobSystem(Config(1u)));
        Jobs::MainThreadContinuationDesc desc;
        desc.function = Increment;
        desc.userData = &completed;
        Jobs::ScheduleMainThreadContinuation(desc);
    });
    starter.join();
    ASSERT_TRUE(acquired.load());
    ASSERT_TRUE(Jobs::TryInitializeJobSystem(Config(4u)));
    EXPECT_EQ(Jobs::DrainMainThreadContinuations(), 1u);
    EXPECT_EQ(completed.load(), 1u);
    Jobs::Internal::ReleaseJobSystem();
    Jobs::ShutdownJobSystem();
    EXPECT_FALSE(Jobs::IsJobSystemInitialized());
}

TEST_F(JobsUnifiedSchedulerTests, NestedGraphDestroyedOnWorkerReleasesItsLease)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(1u)));
    std::atomic<bool> graphSucceeded{false};
    Jobs::JobScheduleDesc desc;
    desc.userData = &graphSucceeded;
    desc.function = [](void* raw)
    {
        namespace TG = NLS::Base::TaskGraph;
        TG::TaskGraphConfig config;
        config.workerCount = 1u;
        TG::TaskGraph graph(config);
        if (!graph.Start())
            return;
        const auto handle = graph.Submit([] {});
        static_cast<std::atomic<bool>*>(raw)->store(graph.Wait(handle) == TG::TaskStatus::Succeeded);
    };
    const auto handle = Jobs::ScheduleJob(desc);
    ASSERT_NE(handle.id, 0u);
    ASSERT_TRUE(WaitUntil([&] { return Jobs::IsCompleted(handle); }));
    EXPECT_TRUE(graphSucceeded.load());
    Jobs::ShutdownJobSystem();
    EXPECT_FALSE(Jobs::IsJobSystemInitialized());
}

TEST_F(JobsUnifiedSchedulerTests, LastLeaseOnWorkerDrainsAndJoinsWithoutPumpingMainContinuations)
{
    ASSERT_TRUE(Jobs::Internal::AcquireJobSystem(Config(1u)));
    struct Data
    {
        std::atomic<bool> entered{false};
        std::atomic<bool> release{false};
        std::promise<void> leaseReleased;
    } data;
    auto released = data.leaseReleased.get_future();
    Jobs::JobScheduleDesc owner;
    owner.userData = &data;
    owner.function = [](void* raw)
    {
        auto& data = *static_cast<Data*>(raw);
        data.entered.store(true);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!data.release.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        Jobs::Internal::ReleaseJobSystem();
        data.leaseReleased.set_value();
    };
    ASSERT_NE(Jobs::ScheduleJob(owner).id, 0u);
    const bool entered = WaitUntil([&] { return data.entered.load(); });
    std::atomic<size_t> computed{0u};
    Jobs::JobScheduleDesc child;
    child.function = Increment;
    child.userData = &computed;
    const auto childHandle = Jobs::ScheduleJob(child);
    std::atomic<size_t> mainCalls{0u};
    Jobs::MainThreadContinuationDesc continuation;
    continuation.function = Increment;
    continuation.userData = &mainCalls;
    const bool queuedContinuation = Jobs::ScheduleMainThreadContinuation(continuation);
    data.release.store(true);
    ASSERT_EQ(released.wait_for(6s), std::future_status::ready);
    Jobs::ShutdownJobSystem(); // waits for the lifecycle join owner, if still running
    EXPECT_TRUE(entered);
    EXPECT_NE(childHandle.id, 0u);
    EXPECT_TRUE(queuedContinuation);
    EXPECT_EQ(computed.load(), 1u);
    EXPECT_EQ(mainCalls.load(), 0u);
    EXPECT_FALSE(Jobs::IsJobSystemInitialized());
}

TEST_F(JobsUnifiedSchedulerTests, CancellationReleasesDeepDependencyChainIteratively)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(0u)));
    std::atomic<size_t> completed{0u};
    std::atomic<size_t> cancelled{0u};
    Jobs::JobScheduleDesc desc;
    desc.function = Increment;
    desc.userData = &completed;
    desc.cancelFunction = Increment;
    desc.cancelUserData = &cancelled;
    const auto first = Jobs::ScheduleJob(desc);
    ASSERT_NE(first.id, 0u);
    auto last = first;
    constexpr size_t count = 10000u;
    for (size_t index = 1u; index < count; ++index)
    {
        last = Jobs::ScheduleJobDepends(desc, last);
        ASSERT_NE(last.id, 0u);
    }
    EXPECT_TRUE(Jobs::RequestCancel(first));
    EXPECT_TRUE(Jobs::IsCompleted(last));
    EXPECT_EQ(completed.load(), 0u);
    EXPECT_EQ(cancelled.load(), count);
}

TEST_F(JobsUnifiedSchedulerTests, DependencyOnlyFenceDiamondCompletesWithoutRecursiveRelease)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(0u)));
    std::atomic<size_t> completed{0u};
    Jobs::JobScheduleDesc desc;
    desc.function = Increment;
    desc.userData = &completed;
    const auto first = Jobs::ScheduleJob(desc);
    const auto left = Jobs::Internal::ScheduleMultiDependencyFence(&first, 1u);
    const auto right = Jobs::Internal::ScheduleMultiDependencyFence(&first, 1u);
    Jobs::JobHandle dependencies[] = {first, left, right};
    const auto joined = Jobs::Internal::ScheduleMultiDependencyFence(dependencies, 3u);
    const auto tail = Jobs::ScheduleJobDepends(desc, joined);
    EXPECT_TRUE(Jobs::ExecuteOneJobQueueJob());
    EXPECT_TRUE(Jobs::ExecuteOneJobQueueJob());
    EXPECT_TRUE(Jobs::IsCompleted(tail));
    EXPECT_EQ(completed.load(), 2u);
}


TEST_F(JobsUnifiedSchedulerTests, BackgroundCapacityRejectsWithoutCleanupAndRecoversAfterCompletion)
{
    auto config = Config(1u);
    config.maxPendingJobCount = 2u;
    ASSERT_TRUE(Jobs::InitializeJobSystem(config));
    struct Blocker
    {
        std::atomic<bool> entered{false};
        std::atomic<bool> release{false};
    } blocker;
    Jobs::BackgroundJobDesc first;
    first.userData = &blocker;
    first.function = [](void* raw)
    {
        auto& blocker = *static_cast<Blocker*>(raw);
        blocker.entered.store(true);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!blocker.release.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
    };
    const auto firstHandle = Jobs::ScheduleBackgroundJob(first);
    const bool entered = WaitUntil([&] { return blocker.entered.load(); });
    std::atomic<size_t> completed{0u};
    std::atomic<size_t> cancelled{0u};
    Jobs::BackgroundJobDesc other;
    other.function = Increment;
    other.userData = &completed;
    other.cancelFunction = Increment;
    other.cancelUserData = &cancelled;
    const auto secondHandle = Jobs::ScheduleBackgroundJob(other);
    const auto rejected = Jobs::ScheduleBackgroundJob(other);
    blocker.release.store(true);
    Jobs::CompleteNoClear(firstHandle);
    Jobs::CompleteNoClear(secondHandle);
    Jobs::DrainAcceptedJobs();
    EXPECT_TRUE(entered);
    EXPECT_NE(firstHandle.id, 0u);
    EXPECT_NE(secondHandle.id, 0u);
    EXPECT_EQ(rejected.id, 0u);
    EXPECT_EQ(cancelled.load(), 0u);
    const auto recovered = Jobs::ScheduleBackgroundJob(other);
    ASSERT_NE(recovered.id, 0u);
    Jobs::CompleteNoClear(recovered);
    EXPECT_EQ(completed.load(), 2u);
    EXPECT_EQ(cancelled.load(), 0u);
}

TEST_F(JobsUnifiedSchedulerTests, BackgroundCapacityIncludesWaitingAndReadyMainContinuations)
{
    auto config = Config(1u);
    config.maxPendingJobCount = 2u;
    ASSERT_TRUE(Jobs::InitializeJobSystem(config));
    struct Blocker
    {
        std::atomic<bool> release{false};
    } blocker;
    Jobs::BackgroundJobDesc background;
    background.userData = &blocker;
    background.function = [](void* raw)
    {
        auto& blocker = *static_cast<Blocker*>(raw);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!blocker.release.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
    };
    const auto handle = Jobs::ScheduleBackgroundJob(background);
    std::atomic<size_t> completed{0u};
    Jobs::MainThreadContinuationDesc continuation;
    continuation.function = Increment;
    continuation.userData = &completed;
    continuation.dependency = handle;
    const bool firstAccepted = Jobs::ScheduleMainThreadContinuation(continuation);
    const bool rejected = Jobs::ScheduleMainThreadContinuation(continuation);
    blocker.release.store(true);
    Jobs::CompleteNoClear(handle);
    EXPECT_NE(handle.id, 0u);
    EXPECT_TRUE(firstAccepted);
    EXPECT_FALSE(rejected);
    continuation.dependency = {};
    ASSERT_TRUE(Jobs::ScheduleMainThreadContinuation(continuation));
    Jobs::BackgroundJobDesc next;
    next.function = Increment;
    next.userData = &completed;
    EXPECT_EQ(Jobs::ScheduleBackgroundJob(next).id, 0u);
    EXPECT_EQ(Jobs::DrainMainThreadContinuations(), 2u);
    EXPECT_EQ(completed.load(), 2u);
    const auto recovered = Jobs::ScheduleBackgroundJob(next);
    ASSERT_NE(recovered.id, 0u);
    Jobs::CompleteNoClear(recovered);
    EXPECT_EQ(completed.load(), 3u);
}


TEST_F(JobsUnifiedSchedulerTests, OwnerTrackedDispatchSharesCapacityAndRejectsWithoutTakingPayload)
{
    auto config = Config(0u);
    config.maxPendingJobCount = 1u;
    ASSERT_TRUE(Jobs::InitializeJobSystem(config));
    std::atomic<size_t> completed{0u}, cancelled{0u};
    Jobs::JobScheduleDesc desc;
    desc.function = Increment;
    desc.userData = &completed;
    desc.cancelFunction = Increment;
    desc.cancelUserData = &cancelled;
    auto ordinary = Jobs::ScheduleJob(desc);
    EXPECT_FALSE(Jobs::Internal::DispatchJob(desc));
    EXPECT_EQ(cancelled.load(), 0u);
    Jobs::Complete(ordinary);
    EXPECT_TRUE(Jobs::Internal::DispatchJob(desc));
    EXPECT_EQ(Jobs::ScheduleJob(desc).id, 0u);
    Jobs::ShutdownJobSystem(Jobs::JobSystemShutdownMode::Immediate);
    EXPECT_EQ(completed.load(), 1u);
    EXPECT_EQ(cancelled.load(), 1u);
}

TEST_F(JobsUnifiedSchedulerTests, OwnerTrackedDispatchExceptionReleasesCapacityWithoutCancelingStartedPayload)
{
    auto config = Config(0u);
    config.maxPendingJobCount = 1u;
    ASSERT_TRUE(Jobs::InitializeJobSystem(config));
    std::atomic<size_t> cancelled{0u}, completed{0u};
    Jobs::JobScheduleDesc desc;
    desc.function = [](void*) { throw std::runtime_error("dispatch failure"); };
    desc.cancelFunction = Increment;
    desc.cancelUserData = &cancelled;
    ASSERT_TRUE(Jobs::Internal::DispatchJob(desc));
    EXPECT_TRUE(Jobs::ExecuteOneJobQueueJob());
    EXPECT_EQ(cancelled.load(), 0u);
    desc.function = Increment;
    desc.userData = &completed;
    ASSERT_TRUE(Jobs::Internal::DispatchJob(desc));
    Jobs::DrainAcceptedJobs();
    EXPECT_EQ(completed.load(), 1u);
    EXPECT_EQ(cancelled.load(), 0u);
}

TEST_F(JobsUnifiedSchedulerTests, OwnerTrackedDispatchDrainWaitsRunningCallbackAndItsDescendant)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(1u)));
    struct Data
    {
        std::atomic<bool> entered{false}, release{false}, accepted{false};
        std::atomic<size_t> completed{0u};
    } data;
    Jobs::JobScheduleDesc desc;
    desc.userData = &data;
    desc.function = [](void* raw)
    {
        auto& data = *static_cast<Data*>(raw);
        data.entered.store(true);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!data.release.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        Jobs::JobScheduleDesc child;
        child.function = Increment;
        child.userData = &data.completed;
        data.accepted.store(Jobs::Internal::DispatchJob(child));
    };
    ASSERT_TRUE(Jobs::Internal::DispatchJob(desc));
    const bool entered = WaitUntil([&] { return data.entered.load(); });
    Jobs::DrainAcceptedJobs([&] { data.release.store(true); });
    EXPECT_TRUE(entered);
    EXPECT_TRUE(data.accepted.load());
    EXPECT_EQ(data.completed.load(), 1u);
}

TEST_F(JobsUnifiedSchedulerTests, OwnerTrackedImmediateShutdownCancelsQueuedOnceAndJoinsRunningCallback)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(1u)));
    struct Data
    {
        std::atomic<bool> entered{false}, release{false}, stopped{false};
        std::atomic<size_t> completed{0u}, cancelled{0u};
    } data;
    Jobs::JobScheduleDesc running;
    running.userData = &data;
    running.function = [](void* raw)
    {
        auto& data = *static_cast<Data*>(raw);
        data.entered.store(true);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!data.release.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        data.completed.fetch_add(1u);
    };
    ASSERT_TRUE(Jobs::Internal::DispatchJob(running));
    const bool entered = WaitUntil([&] { return data.entered.load(); });
    Jobs::JobScheduleDesc queued;
    queued.function = Increment;
    queued.userData = &data.completed;
    queued.cancelFunction = Increment;
    queued.cancelUserData = &data.cancelled;
    const bool accepted = Jobs::Internal::DispatchJob(queued);
    std::thread shutdown([&]
    {
        Jobs::ShutdownJobSystem(Jobs::JobSystemShutdownMode::Immediate);
        data.stopped.store(true);
    });
    const bool cancelled = WaitUntil([&] { return data.cancelled.load() != 0u; });
    EXPECT_FALSE(data.stopped.load());
    data.release.store(true);
    shutdown.join();
    EXPECT_TRUE(entered);
    EXPECT_TRUE(accepted);
    EXPECT_TRUE(cancelled);
    EXPECT_EQ(data.cancelled.load(), 1u);
    EXPECT_EQ(data.completed.load(), 1u);
    EXPECT_TRUE(data.stopped.load());
}

TEST_F(JobsUnifiedSchedulerTests, OwnerTrackedWorkerCanCompleteNestedOrdinaryJob)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(1u)));
    struct Data
    {
        std::atomic<size_t> completed{0u};
        std::atomic<bool> done{false};
    } data;
    Jobs::JobScheduleDesc desc;
    desc.userData = &data;
    desc.function = [](void* raw)
    {
        auto& data = *static_cast<Data*>(raw);
        Jobs::JobScheduleDesc child;
        child.function = Increment;
        child.userData = &data.completed;
        const auto handle = Jobs::ScheduleJob(child);
        Jobs::CompleteNoClear(handle);
        data.done.store(Jobs::GetCompletionStatus(handle) == Jobs::JobCompletionStatus::Succeeded);
    };
    ASSERT_TRUE(Jobs::Internal::DispatchJob(desc));
    EXPECT_TRUE(WaitUntil([&] { return data.done.load(); }));
    Jobs::DrainAcceptedJobs();
    EXPECT_EQ(data.completed.load(), 1u);
}

TEST_F(JobsUnifiedSchedulerTests, OwnerTrackedDispatchRejectsJobDependencies)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(0u)));
    std::atomic<size_t> completed{0u}, cancelled{0u};
    Jobs::JobScheduleDesc desc;
    desc.function = Increment;
    desc.userData = &completed;
    const auto dependency = Jobs::ScheduleJob(desc);
    desc.dependency = dependency;
    desc.cancelFunction = Increment;
    desc.cancelUserData = &cancelled;
    EXPECT_FALSE(Jobs::Internal::DispatchJob(desc));
    EXPECT_EQ(cancelled.load(), 0u);
    Jobs::CompleteNoClear(dependency);
    EXPECT_EQ(completed.load(), 1u);
}


TEST_F(JobsUnifiedSchedulerTests, OrdinarySingleChildSubmissionWakesAnotherWorkerWhileParentBlocks)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(2u)));
    struct Data
    {
        std::promise<void> child;
        std::future<void> childReady = child.get_future();
        std::promise<bool> parent;
    } data;
    auto parentReady = data.parent.get_future();
    Jobs::JobScheduleDesc desc;
    desc.userData = &data;
    desc.function = [](void* raw)
    {
        auto& data = *static_cast<Data*>(raw);
        Jobs::JobScheduleDesc child;
        child.function = [](void* value) { static_cast<std::promise<void>*>(value)->set_value(); };
        child.userData = &data.child;
        const auto handle = Jobs::ScheduleJob(child);
        data.parent.set_value(handle.id != 0u && data.childReady.wait_for(5s) == std::future_status::ready);
    };
    ASSERT_NE(Jobs::ScheduleJob(desc).id, 0u);
    ASSERT_EQ(parentReady.wait_for(6s), std::future_status::ready);
    EXPECT_TRUE(parentReady.get());
    Jobs::DrainAcceptedJobs();
}

TEST_F(JobsUnifiedSchedulerTests, GraphSingleChildSubmissionWakesAnotherWorkerBeforeParentBodyReturns)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(2u)));
    namespace TG = NLS::Base::TaskGraph;
    TG::TaskGraphConfig config;
    config.workerCount = 2u;
    TG::TaskGraph graph(config);
    ASSERT_TRUE(graph.Start());
    std::promise<void> child;
    auto childReady = child.get_future();
    std::atomic<bool> observed{false};
    TG::TaskDesc parent;
    parent.function = [&]
    {
        TG::TaskDesc nested;
        nested.function = [&] { child.set_value(); };
        const auto handle = graph.Submit(std::move(nested));
        observed.store(handle.IsValid() && childReady.wait_for(5s) == std::future_status::ready);
    };
    auto submission = graph.SubmitTracked(std::move(parent));
    EXPECT_TRUE(submission.completion.WaitFor(6s));
    graph.Shutdown();
    EXPECT_TRUE(observed.load());
}


TEST_F(JobsUnifiedSchedulerTests, DependencyFanOutWakesAllAvailableWorkers)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(4u)));
    struct Data
    {
        std::atomic<bool> releaseRoot{false};
        std::atomic<size_t> started{0u}, observedAll{0u}, finished{0u};
    } data;
    Jobs::JobScheduleDesc root;
    root.userData = &data;
    root.function = [](void* raw)
    {
        auto& data = *static_cast<Data*>(raw);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!data.releaseRoot.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
    };
    const auto rootHandle = Jobs::ScheduleJob(root);
    bool accepted = rootHandle.id != 0u;
    Jobs::JobScheduleDesc child;
    child.userData = &data;
    child.function = [](void* raw)
    {
        auto& data = *static_cast<Data*>(raw);
        data.started.fetch_add(1u);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (data.started.load() != 4u && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        if (data.started.load() == 4u)
            data.observedAll.fetch_add(1u);
        data.finished.fetch_add(1u);
    };
    for (size_t i = 0; i < 4u; ++i)
        accepted &= Jobs::ScheduleJobDepends(child, rootHandle).id != 0u;
    data.releaseRoot.store(true);
    // Do not assist from this fifth thread: all four pool workers must wake.
    const bool finished = WaitUntil([&] { return data.finished.load() == 4u; });
    Jobs::DrainAcceptedJobs();
    EXPECT_TRUE(accepted);
    EXPECT_TRUE(finished);
    EXPECT_EQ(data.observedAll.load(), 4u);
}

#if defined(NLS_ENABLE_TEST_HOOKS)
namespace
{
    struct ScopedJobShutdown
    {
        ~ScopedJobShutdown() { Jobs::ShutdownJobSystem(Jobs::JobSystemShutdownMode::Immediate); }
    };

    bool WaitForWaitingForegroundWorkers(const size_t expected)
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            // Count registered waiters, rather than infer sleeping from an idle
            // interval. Registration already arms the persistent wake signal.
            if (Jobs::Internal::GetWaitingForegroundWorkerCountForTesting() == expected)
                return true;
            std::this_thread::yield();
        }
        return Jobs::Internal::GetWaitingForegroundWorkerCountForTesting() == expected;
    }
}

TEST_F(JobsUnifiedSchedulerTests, ParkedWorkerRepeatedlyWakesForIndividualSubmissions)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(1u)));
    std::atomic<size_t> executed{0u};
    for (size_t round = 0u; round < 64u; ++round)
    {
        SCOPED_TRACE(round);
        ASSERT_TRUE(WaitForWaitingForegroundWorkers(1u));
        struct Data
        {
            std::atomic<size_t>* executed;
            std::promise<std::thread::id> completed;
        } data{&executed};
        auto completed = data.completed.get_future();
        Jobs::JobScheduleDesc desc;
        desc.userData = &data;
        desc.function = [](void* raw)
        {
            auto& data = *static_cast<Data*>(raw);
            data.executed->fetch_add(1u);
            data.completed.set_value(std::this_thread::get_id());
        };
        ASSERT_NE(Jobs::ScheduleJob(desc).id, 0u);
        // Do not let Complete/Drain execute the submitted job until the worker
        // has demonstrated that it woke. Drain also protects storage on failure.
        const auto status = completed.wait_for(3s);
        Jobs::DrainAcceptedJobs();
        ASSERT_EQ(status, std::future_status::ready);
        EXPECT_NE(completed.get(), std::this_thread::get_id());
        EXPECT_EQ(executed.load(), round + 1u);
    }
}

TEST_F(JobsUnifiedSchedulerTests, SingleConcurrentBatchWakesAllParkedWorkers)
{
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(4u)));
    ASSERT_TRUE(WaitForWaitingForegroundWorkers(4u));
    struct Data
    {
        std::atomic<size_t> started{0u}, timedOut{0u};
        std::promise<void> allStarted;
        std::promise<void> release;
        std::shared_future<void> releaseFuture;
        std::mutex mutex;
        std::vector<std::thread::id> workerThreads;
    } data;
    data.releaseFuture = data.release.get_future().share();
    auto allStarted = data.allStarted.get_future();
    std::vector<Jobs::JobScheduleDesc> jobs(4u);
    for (auto& desc : jobs)
    {
        desc.userData = &data;
        desc.function = [](void* raw)
        {
            auto& data = *static_cast<Data*>(raw);
            {
                std::lock_guard lock(data.mutex);
                data.workerThreads.push_back(std::this_thread::get_id());
            }
            if (data.started.fetch_add(1u) + 1u == 4u)
                data.allStarted.set_value();
            if (data.releaseFuture.wait_for(5s) != std::future_status::ready)
                data.timedOut.fetch_add(1u);
        };
    }
    const auto batch = Jobs::ScheduleDifferentJobsConcurrent(jobs.data(), jobs.size());
    ASSERT_NE(batch.id, 0u);
    // This fifth thread never assists. All four callbacks must enter before
    // any of them can finish and make the same worker available a second time.
    const auto status = allStarted.wait_for(3s);
    data.release.set_value();
    Jobs::DrainAcceptedJobs();
    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_EQ(data.timedOut.load(), 0u);
    EXPECT_EQ(Jobs::GetCompletionStatus(batch), Jobs::JobCompletionStatus::Succeeded);
    ASSERT_EQ(data.workerThreads.size(), 4u);
    for (size_t first = 0u; first < data.workerThreads.size(); ++first)
    {
        EXPECT_NE(data.workerThreads[first], std::this_thread::get_id());
        for (size_t second = first + 1u; second < data.workerThreads.size(); ++second)
            EXPECT_NE(data.workerThreads[first], data.workerThreads[second]);
    }
}

TEST_F(JobsUnifiedSchedulerTests, ShutdownWakesAllParkedWorkers)
{
    for (const auto mode : {Jobs::JobSystemShutdownMode::DrainAcceptedWork,
                           Jobs::JobSystemShutdownMode::Immediate})
    {
        SCOPED_TRACE(static_cast<int>(mode));
        ASSERT_TRUE(Jobs::InitializeJobSystem(Config(4u)));
        ASSERT_TRUE(WaitForWaitingForegroundWorkers(4u));
        std::promise<void> stopped;
        auto completed = stopped.get_future();
        std::thread shutdown([&stopped, mode]
        {
            Jobs::ShutdownJobSystem(mode);
            stopped.set_value();
        });
        EXPECT_EQ(completed.wait_for(5s), std::future_status::ready);
        // Never detach a lifecycle owner into subsequent tests. CTest's process
        // timeout remains the outer bound if shutdown itself fails to return.
        shutdown.join();
        EXPECT_FALSE(Jobs::IsJobSystemInitialized());
    }
}

TEST_F(JobsUnifiedSchedulerTests, OwnerTrackedRecordsReuseAfterSuccessAndFailure)
{
    std::atomic<size_t> bodies{0u}, cancellations{0u};
    ScopedJobShutdown shutdown;
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(0u)));
    Jobs::JobScheduleDesc desc;
    desc.userData = &bodies;
    desc.cancelFunction = Increment;
    desc.cancelUserData = &cancellations;
    for (size_t index = 0u; index < 256u; ++index)
    {
        desc.function = index % 2u == 0u ? Increment : +[](void* data)
        {
            Increment(data);
            throw std::runtime_error("expected owner-tracked failure");
        };
        ASSERT_TRUE(Jobs::Internal::DispatchJob(desc));
        ASSERT_TRUE(Jobs::ExecuteOneJobQueueJob());
    }
    EXPECT_EQ(bodies.load(), 256u);
    EXPECT_EQ(cancellations.load(), 0u);
    const auto stats = Jobs::Internal::GetDispatchReuseStatsForTesting();
    EXPECT_EQ(stats.allocations, 1u);
    EXPECT_EQ(stats.reuses, 255u);
    EXPECT_EQ(stats.cached, 1u);
}

TEST_F(JobsUnifiedSchedulerTests, ReusedDispatchPreservesCapacityAndPendingCancellation)
{
    std::atomic<size_t> bodies{0u}, cancellations{0u};
    ScopedJobShutdown shutdown;
    auto config = Config(0u);
    config.maxPendingJobCount = 2u;
    ASSERT_TRUE(Jobs::InitializeJobSystem(config));
    Jobs::JobScheduleDesc desc;
    desc.function = Increment;
    desc.userData = &bodies;
    desc.cancelFunction = Increment;
    desc.cancelUserData = &cancellations;
    ASSERT_TRUE(Jobs::Internal::DispatchJob(desc));
    ASSERT_TRUE(Jobs::ExecuteOneJobQueueJob());
    ASSERT_TRUE(Jobs::Internal::DispatchJob(desc)); // Reuses a consumed callback record.
    ASSERT_TRUE(Jobs::Internal::DispatchJob(desc));
    EXPECT_FALSE(Jobs::Internal::DispatchJob(desc));
    EXPECT_EQ(Jobs::Internal::GetDispatchReuseStatsForTesting().reuses, 1u);
    Jobs::ShutdownJobSystem(Jobs::JobSystemShutdownMode::Immediate);
    EXPECT_EQ(bodies.load(), 1u);
    EXPECT_EQ(cancellations.load(), 2u);
}

TEST_F(JobsUnifiedSchedulerTests, CompletedDispatchCacheIsBoundedAndClearedOnRestart)
{
    std::atomic<size_t> bodies{0u};
    ScopedJobShutdown shutdown;
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(0u)));
    Jobs::JobScheduleDesc desc;
    desc.function = Increment;
    desc.userData = &bodies;
    for (size_t index = 0u; index < 256u; ++index)
        ASSERT_TRUE(Jobs::Internal::DispatchJob(desc));
    while (Jobs::ExecuteOneJobQueueJob()) {}
    const auto stats = Jobs::Internal::GetDispatchReuseStatsForTesting();
    EXPECT_EQ(bodies.load(), 256u);
    EXPECT_EQ(stats.allocations, 256u);
    EXPECT_GT(stats.cached, 0u);
    EXPECT_LE(stats.cached, 64u);
    for (size_t index = 0u; index < 128u; ++index)
    {
        ASSERT_TRUE(Jobs::Internal::DispatchJob(desc));
        ASSERT_TRUE(Jobs::ExecuteOneJobQueueJob());
    }
    EXPECT_EQ(Jobs::Internal::GetDispatchReuseStatsForTesting().allocations, 256u);
    EXPECT_EQ(bodies.load(), 384u);
    Jobs::ShutdownJobSystem();
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(0u)));
    const auto restarted = Jobs::Internal::GetDispatchReuseStatsForTesting();
    EXPECT_EQ(restarted.allocations, 0u);
    EXPECT_EQ(restarted.reuses, 0u);
    EXPECT_EQ(restarted.cached, 0u);
}

TEST_F(JobsUnifiedSchedulerTests, PublicJobHandlesStayValidAcrossAnonymousRecordReuse)
{
    std::atomic<size_t> bodies{0u};
    ScopedJobShutdown shutdown;
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(0u)));
    Jobs::JobScheduleDesc desc;
    desc.function = Increment;
    desc.userData = &bodies;
    const auto completed = Jobs::ScheduleJob(desc);
    ASSERT_NE(completed.id, 0u);
    Jobs::CompleteNoClear(completed);
    for (size_t index = 0u; index < 128u; ++index)
    {
        ASSERT_TRUE(Jobs::Internal::DispatchJob(desc));
        ASSERT_TRUE(Jobs::ExecuteOneJobQueueJob());
    }
    EXPECT_EQ(Jobs::GetCompletionStatus(completed), Jobs::JobCompletionStatus::Succeeded);
    auto dependent = Jobs::ScheduleJobDepends(desc, completed);
    ASSERT_NE(dependent.id, 0u);
    Jobs::Complete(dependent);
    EXPECT_EQ(bodies.load(), 130u);
}

TEST_F(JobsUnifiedSchedulerTests, ReusedDispatchRefreshesDiagnosticIdentityAndOwnedNames)
{
    std::atomic<size_t> bodies{0u};
    ScopedJobShutdown shutdown;
    Jobs::ResetJobDiagnosticsForTesting();
    auto config = Config(0u);
    config.enableDiagnostics = true;
    ASSERT_TRUE(Jobs::InitializeJobSystem(config));
    std::vector<std::string> names;
    for (size_t index = 0u; index < 4u; ++index)
    {
        names.push_back("dispatch-reuse-" + std::to_string(index));
        std::string temporary = names.back();
        Jobs::JobScheduleDesc desc;
        desc.function = Increment;
        desc.userData = &bodies;
        desc.debugName = temporary.c_str();
        ASSERT_TRUE(Jobs::Internal::DispatchJob(desc));
        temporary.assign("mutated caller storage");
        ASSERT_TRUE(Jobs::ExecuteOneJobQueueJob());
    }
    const auto snapshot = Jobs::CopyJobDiagnosticSnapshot();
    uint64_t previousId = 0u;
    for (const auto& name : names)
    {
        std::vector<Jobs::JobDiagnosticRecord> records;
        for (const auto& record : snapshot.recentJobs)
            if (record.debugName == name)
                records.push_back(record);
        ASSERT_EQ(records.size(), 3u);
        EXPECT_EQ(records[0].state, Jobs::JobLifecycleState::Queued);
        EXPECT_EQ(records[1].state, Jobs::JobLifecycleState::Running);
        EXPECT_EQ(records[2].state, Jobs::JobLifecycleState::Completed);
        EXPECT_GT(records[0].id, previousId);
        EXPECT_EQ(records[1].id, records[0].id);
        EXPECT_EQ(records[2].id, records[0].id);
        previousId = records[0].id;
    }
    EXPECT_EQ(bodies.load(), 4u);
    EXPECT_EQ(Jobs::Internal::GetDispatchReuseStatsForTesting().allocations, 1u);
}

TEST_F(JobsUnifiedSchedulerTests, SingleWorkerGraphChainReusesDispatchRecordsWithZeroRetention)
{
    namespace TG = NLS::Base::TaskGraph;
    std::atomic<size_t> bodies{0u};
    ScopedJobShutdown shutdown;
    ASSERT_TRUE(Jobs::InitializeJobSystem(Config(1u)));
    TG::TaskGraphConfig config{1u};
    config.retainedTerminalTaskCount = 0u;
    TG::TaskGraph graph(config);
    ASSERT_TRUE(graph.RegisterCurrentThread(TG::NamedThread::Game));
    TG::TaskDesc root;
    root.function = [] {};
    root.targetThread = TG::NamedThread::Game;
    auto tail = graph.SubmitTracked(std::move(root));
    constexpr size_t length = 2000u;
    for (size_t index = 0u; index < length; ++index)
    {
        TG::TaskDesc node;
        node.function = [&] { ++bodies; };
        node.prerequisites = {tail.handle};
        tail = graph.SubmitTracked(std::move(node));
        ASSERT_TRUE(tail.handle.IsValid());
    }
    EXPECT_TRUE(tail.completion.WaitFor(5s));
    graph.Shutdown();
    Jobs::DrainAcceptedJobs();
    EXPECT_EQ(bodies.load(), length);
    const auto stats = Jobs::Internal::GetDispatchReuseStatsForTesting();
    EXPECT_LT(stats.allocations, 16u);
    EXPECT_GE(stats.reuses, length - 16u);
    RecordProperty("dispatch_allocations", std::to_string(stats.allocations));
    RecordProperty("dispatch_reuses", std::to_string(stats.reuses));
}
#endif
