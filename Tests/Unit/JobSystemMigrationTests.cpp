#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "Core/EditorBackgroundTaskTracker.h"
#include "Core/RecentBackgroundWorkGate.h"
#include "Debug/Logger.h"
#include "Jobs/JobSystem.h"

namespace
{
}

class JobSystemMigrationBehaviorTests : public ::testing::Test
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

TEST_F(JobSystemMigrationBehaviorTests, EditorBackgroundTaskTrackerRunsTasksAndDrainsOnDestruction)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    {
        NLS::Editor::Core::EditorBackgroundTaskTracker tracker(4u);
        EXPECT_TRUE(tracker.Track([&counter] { counter.fetch_add(1, std::memory_order_acq_rel); }));
        EXPECT_EQ(counter.load(std::memory_order_acquire), 0);
    }

    EXPECT_EQ(counter.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemMigrationBehaviorTests, EditorBackgroundTaskTrackerQueuesRequiredWorkPastSoftCapacityAndRejectsAfterStop)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<bool> release = false;
    std::atomic<int> started = 0;
    std::atomic<int> secondStarted = 0;
    NLS::Editor::Core::EditorBackgroundTaskTracker tracker(1u);
    EXPECT_TRUE(tracker.Track(
        [&]
        {
            started.fetch_add(1, std::memory_order_acq_rel);
            while (!release.load(std::memory_order_acquire))
                std::this_thread::yield();
        }));

    for (int attempt = 0; attempt < 200 && started.load(std::memory_order_acquire) == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(started.load(std::memory_order_acquire), 1);

    EXPECT_TRUE(tracker.Track(
        [&]
        {
            secondStarted.fetch_add(1, std::memory_order_acq_rel);
        }));
    release.store(true, std::memory_order_release);
    tracker.StopAndComplete();
    EXPECT_EQ(secondStarted.load(std::memory_order_acquire), 1);
    EXPECT_FALSE(tracker.Track([] {}));
}

TEST_F(JobSystemMigrationBehaviorTests, EditorBackgroundTaskTrackerRejectsRequiredWorkAtHardCapacity)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<bool> release = false;
    std::atomic<int> started = 0;
    std::atomic<int> completed = 0;
    NLS::Editor::Core::EditorBackgroundTaskTracker tracker(1u, 2u);
    const auto blockingTask = [&]
    {
        started.fetch_add(1, std::memory_order_acq_rel);
        while (!release.load(std::memory_order_acquire))
            std::this_thread::yield();
        completed.fetch_add(1, std::memory_order_acq_rel);
    };

    EXPECT_TRUE(tracker.Track(blockingTask));
    EXPECT_TRUE(tracker.Track(blockingTask));
    EXPECT_FALSE(tracker.Track([] {}))
        << "Required editor work may exceed the soft capacity, but the tracker must still enforce a hard bound.";

    release.store(true, std::memory_order_release);
    tracker.StopAndComplete();
    EXPECT_EQ(started.load(std::memory_order_acquire), 2);
    EXPECT_EQ(completed.load(std::memory_order_acquire), 2);
}

TEST_F(JobSystemMigrationBehaviorTests, EditorBackgroundTaskTrackerRejectsOpportunisticWorkWithoutQueueFullWarning)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<bool> release = false;
    std::atomic<int> started = 0;
    NLS::Editor::Core::EditorBackgroundTaskTracker tracker(1u);
    EXPECT_TRUE(tracker.Track(
        [&]
        {
            started.fetch_add(1, std::memory_order_acq_rel);
            while (!release.load(std::memory_order_acquire))
                std::this_thread::yield();
        }));

    for (int attempt = 0; attempt < 200 && started.load(std::memory_order_acquire) == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(started.load(std::memory_order_acquire), 1);

    size_t queueCapacityWarnings = 0u;
    const auto listener = NLS::Debug::Logger::LogEvent +=
        [&queueCapacityWarnings](const NLS::Debug::LogData& log)
        {
            const bool editorBackgroundQueueWarning =
                log.message.find("Editor background task queue") != std::string::npos ||
                log.message.find("Editor background task rejected by the shared JobSystem") != std::string::npos;
            if (log.logLevel == NLS::Debug::ELogLevel::LOG_WARNING && editorBackgroundQueueWarning)
            {
                ++queueCapacityWarnings;
            }
        };

    EXPECT_FALSE(tracker.TrackOpportunistic([] {}));
    NLS::Debug::Logger::LogEvent -= listener;

    release.store(true, std::memory_order_release);
    tracker.StopAndComplete();
    EXPECT_EQ(queueCapacityWarnings, 0u);
}

TEST_F(JobSystemMigrationBehaviorTests, RecentBackgroundWorkGateSuppressesCompletedKeysUntilTtlExpires)
{
    using Gate = NLS::Editor::Core::RecentBackgroundWorkGate;
    const auto ttl = std::chrono::seconds(3);
    Gate gate(8u, ttl);

    const auto start = Gate::Clock::time_point(std::chrono::seconds(10));
    EXPECT_TRUE(gate.TryBegin("prefab-a", start));
    EXPECT_TRUE(gate.IsInFlight("prefab-a"));
    EXPECT_FALSE(gate.TryBegin("prefab-a", start + std::chrono::milliseconds(1)));

    gate.End("prefab-a");
    EXPECT_FALSE(gate.IsInFlight("prefab-a"));
    EXPECT_FALSE(gate.TryBegin("prefab-a", start + std::chrono::seconds(2)));
    EXPECT_TRUE(gate.TryBegin("prefab-a", start + ttl + std::chrono::milliseconds(1)));
}

TEST_F(JobSystemMigrationBehaviorTests, RecentBackgroundWorkGatePrunesToCapacity)
{
    using Gate = NLS::Editor::Core::RecentBackgroundWorkGate;
    Gate gate(2u, std::chrono::minutes(5));

    const auto start = Gate::Clock::time_point(std::chrono::seconds(20));
    EXPECT_TRUE(gate.TryBegin("prefab-a", start));
    gate.End("prefab-a");
    EXPECT_TRUE(gate.TryBegin("prefab-b", start + std::chrono::milliseconds(1)));
    gate.End("prefab-b");
    EXPECT_TRUE(gate.TryBegin("prefab-c", start + std::chrono::milliseconds(2)));
    gate.End("prefab-c");

    EXPECT_LE(gate.EntryCountForTesting(), 2u);
    EXPECT_TRUE(gate.TryBegin("prefab-a", start + std::chrono::milliseconds(3)));
}

TEST_F(JobSystemMigrationBehaviorTests, RecentBackgroundWorkGateCompletionReleasesInFlightWhenScopeThrows)
{
    using Gate = NLS::Editor::Core::RecentBackgroundWorkGate;
    const auto ttl = std::chrono::seconds(3);
    Gate gate(8u, ttl);

    const auto start = Gate::Clock::time_point(std::chrono::seconds(30));
    ASSERT_TRUE(gate.TryBegin("prefab-a", start));
    EXPECT_TRUE(gate.IsInFlight("prefab-a"));

    try
    {
        auto completion = gate.CompleteOnScopeExit("prefab-a");
        throw std::runtime_error("simulated preload failure");
    }
    catch (const std::runtime_error&)
    {
    }

    EXPECT_FALSE(gate.IsInFlight("prefab-a"));
    EXPECT_FALSE(gate.TryBegin("prefab-a", start + std::chrono::seconds(1)));
    EXPECT_TRUE(gate.TryBegin("prefab-a", start + ttl + std::chrono::milliseconds(1)));
}
