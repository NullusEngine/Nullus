#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "Core/EditorBackgroundTaskTracker.h"
#include "Jobs/JobSystem.h"

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

TEST_F(JobSystemMigrationBehaviorTests, EditorBackgroundTaskTrackerRejectsAfterStopAndHonorsCapacity)
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

    EXPECT_FALSE(tracker.Track([] {}));
    release.store(true, std::memory_order_release);
    tracker.StopAndComplete();
    EXPECT_FALSE(tracker.Track([] {}));
}
