#include <gtest/gtest.h>

#include <type_traits>

#include "Rendering/RHI/Backends/DX12/DX12PresentPolicy.h"
#include "UI/ImGuiExtensions/TimelineProfiler/Profiler.h"
#include "UI/Profiling/TimelineProfilerSink.h"

TEST(TimelineProfilerGpuLifecycleTests, BuildConfigurationExposesExpectedProfilerHelpers)
{
#if NLS_ENABLE_TIMELINE_PROFILER
    EXPECT_FALSE(TimelineProfilerDetail::ShouldUnregisterCommandListDestructionCallback(true, 1u));
    EXPECT_TRUE(TimelineProfilerDetail::ShouldUnregisterCommandListDestructionCallback(false, 1u));
    EXPECT_FALSE(TimelineProfilerDetail::ShouldPublishGpuQueryPair(false));
    EXPECT_TRUE(TimelineProfilerDetail::ShouldPublishGpuQueryPair(true));
    EXPECT_FALSE(TimelineProfilerDetail::ShouldAdvanceGpuProfilerFrame(true, false));
    EXPECT_FALSE(TimelineProfilerDetail::ShouldAdvanceGpuProfilerFrame(false, true));
    EXPECT_TRUE(TimelineProfilerDetail::ShouldAdvanceGpuProfilerFrame(false, false));
    EXPECT_GT(TimelineProfilerDetail::ComputeTimelineWheelZoomScale(5.0f, 1.0f), 5.0f);
    EXPECT_LT(TimelineProfilerDetail::ComputeTimelineWheelZoomScale(5.0f, -1.0f), 5.0f);
    EXPECT_FLOAT_EQ(TimelineProfilerDetail::ComputeTimelineWheelZoomScale(1.0f, -20.0f), 1.0f);
    EXPECT_FLOAT_EQ(TimelineProfilerDetail::ComputeTimelineWheelZoomScale(100.0f, 20.0f), 100.0f);
#else
    SUCCEED() << "TimelineProfiler is disabled in this build.";
#endif
}

TEST(DX12PresentPolicyTests, VsyncControlsPresentSyncInterval)
{
    EXPECT_EQ(NLS::Render::Backend::ResolveDX12PresentSyncInterval(true), 1u);
    EXPECT_EQ(NLS::Render::Backend::ResolveDX12PresentSyncInterval(false), 0u);
}

TEST(TimelineProfilerGpuLifecycleTests, TimelineSinkGpuProfilerOwnershipIsNonCopyableAndNonMovable)
{
#if NLS_ENABLE_TIMELINE_PROFILER
    using NLS::Base::Profiling::TimelineProfilerSink;

    EXPECT_FALSE(std::is_copy_constructible_v<TimelineProfilerSink>);
    EXPECT_FALSE(std::is_copy_assignable_v<TimelineProfilerSink>);
    EXPECT_FALSE(std::is_move_constructible_v<TimelineProfilerSink>);
    EXPECT_FALSE(std::is_move_assignable_v<TimelineProfilerSink>);
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, ResolveFenceValuesTreatInitialFenceAsIncomplete)
{
#if NLS_ENABLE_TIMELINE_PROFILER
    EXPECT_EQ(TimelineProfilerDetail::GetGpuProfilerResolveFenceValue(0u), 1u);
    EXPECT_EQ(TimelineProfilerDetail::GetGpuProfilerResolveFenceValue(4u), 5u);

    EXPECT_FALSE(TimelineProfilerDetail::IsGpuProfilerFrameFenceComplete(0u, 0u));
    EXPECT_TRUE(TimelineProfilerDetail::IsGpuProfilerFrameFenceComplete(1u, 0u));
    EXPECT_FALSE(TimelineProfilerDetail::IsGpuProfilerFrameFenceComplete(4u, 4u));
    EXPECT_TRUE(TimelineProfilerDetail::IsGpuProfilerFrameFenceComplete(5u, 4u));

    EXPECT_FALSE(TimelineProfilerDetail::ShouldWaitForGpuProfilerResolveFence(0u, 0u));
    EXPECT_TRUE(TimelineProfilerDetail::ShouldWaitForGpuProfilerResolveFence(0u, 1u));
    EXPECT_FALSE(TimelineProfilerDetail::ShouldWaitForGpuProfilerResolveFence(1u, 1u));
    EXPECT_TRUE(TimelineProfilerDetail::ShouldReleaseGpuProfilerResolveResourcesAfterFenceWait(false, false));
    EXPECT_FALSE(TimelineProfilerDetail::ShouldReleaseGpuProfilerResolveResourcesAfterFenceWait(true, false));
    EXPECT_TRUE(TimelineProfilerDetail::ShouldReleaseGpuProfilerResolveResourcesAfterFenceWait(true, true));
    EXPECT_GT(TimelineProfilerDetail::GetGpuProfilerResolveFenceWaitTimeoutMilliseconds(), 0u);
    EXPECT_FALSE(TimelineProfilerDetail::CanReleaseGpuProfilerWithPendingCommandListQueries(true));
    EXPECT_TRUE(TimelineProfilerDetail::CanReleaseGpuProfilerWithPendingCommandListQueries(false));
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}
