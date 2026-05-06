#include <gtest/gtest.h>

#include "Rendering/RHI/Backends/DX12/DX12PresentPolicy.h"
#include "UI/ImGuiExtensions/TimelineProfiler/Profiler.h"

TEST(TimelineProfilerGpuLifecycleTests, BuildConfigurationExposesExpectedProfilerHelpers)
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER)
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
