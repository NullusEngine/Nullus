#include <gtest/gtest.h>

#include "Rendering/RHI/Backends/DX12/DX12UIFrameFenceTracker.h"

namespace
{
    TEST(DX12UIFrameFenceTrackerTests, FirstUseOfBackbufferDoesNotRequireWait)
    {
        NLS::Render::RHI::DX12::DX12UIFrameFenceTracker tracker;
        tracker.ResetBackbufferCount(2u);

        const auto reuse = tracker.ResolveReuseWait(0u, 0u);

        EXPECT_FALSE(reuse.shouldWait);
        EXPECT_EQ(reuse.fenceValue, 0u);
    }

    TEST(DX12UIFrameFenceTrackerTests, ReusingIncompleteBackbufferRequiresWaitForPreviousValue)
    {
        NLS::Render::RHI::DX12::DX12UIFrameFenceTracker tracker;
        tracker.ResetBackbufferCount(2u);
        tracker.RecordSubmitted(1u, 7u);

        const auto reuse = tracker.ResolveReuseWait(1u, 6u);

        EXPECT_TRUE(reuse.shouldWait);
        EXPECT_EQ(reuse.fenceValue, 7u);
    }

    TEST(DX12UIFrameFenceTrackerTests, ReusingCompletedBackbufferDoesNotRequireWait)
    {
        NLS::Render::RHI::DX12::DX12UIFrameFenceTracker tracker;
        tracker.ResetBackbufferCount(2u);
        tracker.RecordSubmitted(1u, 7u);

        const auto reuse = tracker.ResolveReuseWait(1u, 7u);

        EXPECT_FALSE(reuse.shouldWait);
        EXPECT_EQ(reuse.fenceValue, 7u);
    }

    TEST(DX12UIFrameFenceTrackerTests, LatestSubmittedFenceValueIsExposedForPresent)
    {
        NLS::Render::RHI::DX12::DX12UIFrameFenceTracker tracker;
        tracker.ResetBackbufferCount(2u);

        tracker.RecordSubmitted(0u, 3u);
        tracker.RecordSubmitted(1u, 4u);

        EXPECT_EQ(tracker.GetLastSubmittedFenceValue(), 4u);
    }

    TEST(DX12UIFrameFenceTrackerTests, ResetBackbufferCountClearsReuseState)
    {
        NLS::Render::RHI::DX12::DX12UIFrameFenceTracker tracker;
        tracker.ResetBackbufferCount(2u);
        tracker.RecordSubmitted(0u, 9u);

        tracker.ResetBackbufferCount(3u);
        const auto reuse = tracker.ResolveReuseWait(0u, 0u);

        EXPECT_FALSE(reuse.shouldWait);
        EXPECT_EQ(reuse.fenceValue, 0u);
        EXPECT_EQ(tracker.GetLastSubmittedFenceValue(), 0u);
    }
}
