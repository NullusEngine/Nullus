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

    TEST(DX12UIFrameFenceTrackerTests, CompletedAllocatorCanBeSelectedWhenBackbufferFenceIsIncomplete)
    {
        NLS::Render::RHI::DX12::DX12UIFrameFenceTracker tracker;
        tracker.ResetBackbufferCount(2u);
        tracker.ResetAllocatorCount(3u);
        tracker.RecordSubmitted(0u, 8u, 0u);
        tracker.RecordSubmitted(1u, 9u, 1u);

        const auto selection = tracker.ResolveAllocatorReuse(0u);

        EXPECT_FALSE(selection.shouldWait);
        ASSERT_TRUE(selection.allocatorIndex.has_value());
        EXPECT_EQ(*selection.allocatorIndex, 2u);

        const auto oldBackbufferReuse = tracker.ResolveReuseWait(0u, 0u);
        EXPECT_TRUE(oldBackbufferReuse.shouldWait);
        EXPECT_EQ(oldBackbufferReuse.fenceValue, 8u);
    }

    TEST(DX12UIFrameFenceTrackerTests, ExhaustedAllocatorPoolRequestsOldestIncompleteFence)
    {
        NLS::Render::RHI::DX12::DX12UIFrameFenceTracker tracker;
        tracker.ResetAllocatorCount(3u);
        tracker.RecordAllocatorSubmitted(0u, 12u);
        tracker.RecordAllocatorSubmitted(1u, 7u);
        tracker.RecordAllocatorSubmitted(2u, 9u);

        const auto selection = tracker.ResolveAllocatorReuse(6u);

        EXPECT_TRUE(selection.shouldWait);
        EXPECT_EQ(selection.fenceValue, 7u);
        ASSERT_TRUE(selection.allocatorIndex.has_value());
        EXPECT_EQ(*selection.allocatorIndex, 1u);
    }

    TEST(DX12UIFrameFenceTrackerTests, CompletedAllocatorIsPreferredBeforeWaiting)
    {
        NLS::Render::RHI::DX12::DX12UIFrameFenceTracker tracker;
        tracker.ResetAllocatorCount(3u);
        tracker.RecordAllocatorSubmitted(0u, 12u);
        tracker.RecordAllocatorSubmitted(1u, 7u);
        tracker.RecordAllocatorSubmitted(2u, 9u);

        const auto selection = tracker.ResolveAllocatorReuse(7u);

        EXPECT_FALSE(selection.shouldWait);
        ASSERT_TRUE(selection.allocatorIndex.has_value());
        EXPECT_EQ(*selection.allocatorIndex, 1u);
        EXPECT_EQ(selection.fenceValue, 7u);
    }

    TEST(DX12UIFrameFenceTrackerTests, ResetAllocatorCountClearsAllocatorReuseState)
    {
        NLS::Render::RHI::DX12::DX12UIFrameFenceTracker tracker;
        tracker.ResetAllocatorCount(2u);
        tracker.RecordAllocatorSubmitted(0u, 5u);
        tracker.RecordAllocatorSubmitted(1u, 6u);

        tracker.ResetAllocatorCount(3u);
        const auto selection = tracker.ResolveAllocatorReuse(0u);

        EXPECT_FALSE(selection.shouldWait);
        ASSERT_TRUE(selection.allocatorIndex.has_value());
        EXPECT_EQ(*selection.allocatorIndex, 0u);
        EXPECT_EQ(selection.fenceValue, 0u);
    }

    TEST(DX12UIFrameFenceTrackerTests, UiFenceWaitTimeoutIsFinite)
    {
        EXPECT_NE(NLS::Render::RHI::DX12::GetDX12UIFenceWaitTimeoutMilliseconds(), 0xFFFFFFFFu);
        EXPECT_GT(NLS::Render::RHI::DX12::GetDX12UIFenceWaitTimeoutMilliseconds(), 0u);
    }
}
