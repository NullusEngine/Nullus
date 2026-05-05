#include <gtest/gtest.h>

#include "UI/ImGuiExtensions/TimelineProfiler/Profiler.h"

TEST(TimelineProfilerGpuLifecycleTests, CommandListStateDoesNotUnregisterDuringDestructionCallback)
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    EXPECT_FALSE(TimelineProfilerDetail::ShouldUnregisterCommandListDestructionCallback(true, 1u));
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, CommandListStateUnregistersDuringProfilerShutdown)
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    EXPECT_TRUE(TimelineProfilerDetail::ShouldUnregisterCommandListDestructionCallback(false, 1u));
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, InvalidGpuQueryPairsAreSkippedDuringReadback)
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    EXPECT_FALSE(TimelineProfilerDetail::ShouldPublishGpuQueryPair(false));
    EXPECT_TRUE(TimelineProfilerDetail::ShouldPublishGpuQueryPair(true));
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, MouseWheelDirectlyAdjustsTimelineZoomScale)
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    EXPECT_GT(TimelineProfilerDetail::ComputeTimelineWheelZoomScale(5.0f, 1.0f), 5.0f);
    EXPECT_LT(TimelineProfilerDetail::ComputeTimelineWheelZoomScale(5.0f, -1.0f), 5.0f);
    EXPECT_FLOAT_EQ(TimelineProfilerDetail::ComputeTimelineWheelZoomScale(1.0f, -20.0f), 1.0f);
    EXPECT_FLOAT_EQ(TimelineProfilerDetail::ComputeTimelineWheelZoomScale(100.0f, 20.0f), 100.0f);
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}
