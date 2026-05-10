#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Profiling/Profiler.h"
#include "UI/Profiling/TimelineProfilerSink.h"
#include "Profiling/TracyProfiler.h"

namespace
{
struct RecordedEvent
{
    std::string phase;
    std::string name;
    std::string threadName;
};

class RecordingProfilerDestination final : public NLS::Base::Profiling::IProfilerDestination
{
public:
    explicit RecordingProfilerDestination(
        NLS::Base::Profiling::ProfilerDestinationId id = NLS::Base::Profiling::ProfilerDestinationId::Timeline)
        : m_state { id, true, NLS::Base::Profiling::ProfilerAvailability::Available,
              NLS::Base::Profiling::ProfilerCapability_CPUScopes, "" }
    {
    }

    void BeginScope(const NLS::Base::Profiling::ProfilerScopeEvent& event) override
    {
        events.push_back({ "begin", event.name, event.threadName });
    }

    void EndScope(const NLS::Base::Profiling::ProfilerScopeEvent& event) override
    {
        events.push_back({ "end", event.name, event.threadName });
    }

    void BeginGpuScope(const NLS::Base::Profiling::ProfilerGpuScopeEvent& event) override
    {
        events.push_back({ "gpu-begin", event.name, event.threadName });
    }

    void EndGpuScope(const NLS::Base::Profiling::ProfilerGpuScopeEvent& event) override
    {
        events.push_back({ "gpu-end", event.name, event.threadName });
    }

    void InitializeGpuContext(const NLS::Base::Profiling::ProfilerGpuContextEvent& event) override
    {
        ++gpuContextInitializeCalls;
        lastGpuContextQueueCount = event.nativeCommandQueues.size();
    }

    NLS::Base::Profiling::ProfilerDestinationState GetState() const override
    {
        return m_state;
    }

    NLS::Base::Profiling::ProfilerDestinationState m_state;
    std::vector<RecordedEvent> events;
    int gpuContextInitializeCalls = 0;
    size_t lastGpuContextQueueCount = 0u;
};

class UnavailableProfilerDestination final : public NLS::Base::Profiling::IProfilerDestination
{
public:
    void BeginScope(const NLS::Base::Profiling::ProfilerScopeEvent&) override
    {
        ++beginCalls;
    }

    void EndScope(const NLS::Base::Profiling::ProfilerScopeEvent&) override
    {
        ++endCalls;
    }

    void BeginGpuScope(const NLS::Base::Profiling::ProfilerGpuScopeEvent&) override
    {
        ++beginCalls;
    }

    void EndGpuScope(const NLS::Base::Profiling::ProfilerGpuScopeEvent&) override
    {
        ++endCalls;
    }

    NLS::Base::Profiling::ProfilerDestinationState GetState() const override
    {
        return {
            NLS::Base::Profiling::ProfilerDestinationId::Tracy,
            true,
            NLS::Base::Profiling::ProfilerAvailability::Unavailable,
            NLS::Base::Profiling::ProfilerCapability_CPUScopes,
            "Unavailable for test"
        };
    }

    int beginCalls = 0;
    int endCalls = 0;
};

class ProfilerDestinationTest : public testing::Test
{
protected:
    void SetUp() override
    {
        NLS::Base::Profiling::Profiler::ResetForTesting();
    }

    void TearDown() override
    {
        NLS::Base::Profiling::Profiler::ResetForTesting();
    }
};
}

TEST_F(ProfilerDestinationTest, RegistersAndRoutesScopeToAvailableDestination)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination destination;
    Profiler::RegisterDestination(destination);
    Profiler::SetEnabled(true);

    const auto scope = Profiler::BeginScope("Destination Scope", __FUNCTION__);
    Profiler::EndScope(scope);

    ASSERT_EQ(destination.events.size(), 2u);
    EXPECT_EQ(destination.events[0].phase, "begin");
    EXPECT_EQ(destination.events[0].name, "Destination Scope");
    EXPECT_EQ(destination.events[1].phase, "end");
    EXPECT_EQ(destination.events[1].name, "Destination Scope");

    const auto stats = Profiler::GetSessionStats();
    EXPECT_EQ(stats.acceptedEventCount, 1u);
    EXPECT_EQ(stats.droppedEventCount, 0u);
}

TEST_F(ProfilerDestinationTest, RoutesScopeToEveryAvailableDestination)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination first(ProfilerDestinationId::Tracy);
    RecordingProfilerDestination second(ProfilerDestinationId::Timeline);
    Profiler::RegisterDestination(first);
    Profiler::RegisterDestination(second);
    Profiler::SetEnabled(true);

    const auto scope = Profiler::BeginScope("Shared Scope", __FUNCTION__);
    Profiler::EndScope(scope);

    ASSERT_GE(first.events.size(), 2u);
    ASSERT_GE(second.events.size(), 2u);
    EXPECT_EQ(first.events[0].name, "Shared Scope");
    EXPECT_EQ(second.events[0].name, "Shared Scope");
    EXPECT_EQ(first.events.back().name, "Shared Scope");
    EXPECT_EQ(second.events.back().name, "Shared Scope");
}

TEST_F(ProfilerDestinationTest, UnregisteredDestinationStopsReceivingScopeEvents)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination destination;
    Profiler::RegisterDestination(destination);
    Profiler::SetEnabled(true);

    EXPECT_EQ(Profiler::GetDestinationCountForTesting(), 1u);

    Profiler::UnregisterDestination(destination);

    EXPECT_EQ(Profiler::GetDestinationCountForTesting(), 0u);

    const auto scope = Profiler::BeginScope("After Unregister", __FUNCTION__);
    Profiler::EndScope(scope);

    EXPECT_TRUE(destination.events.empty());
}

TEST_F(ProfilerDestinationTest, ScopeEndOnlyRoutesToDestinationsThatAcceptedBegin)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination earlyDestination;
    Profiler::RegisterDestination(earlyDestination);
    Profiler::SetEnabled(true);

    const auto scope = Profiler::BeginScope("In Flight Scope", __FUNCTION__);

    RecordingProfilerDestination lateDestination;
    Profiler::RegisterDestination(lateDestination);

    Profiler::EndScope(scope);

    ASSERT_EQ(earlyDestination.events.size(), 2u);
    EXPECT_EQ(earlyDestination.events[0].phase, "begin");
    EXPECT_EQ(earlyDestination.events[1].phase, "end");
    EXPECT_TRUE(lateDestination.events.empty());
}

TEST_F(ProfilerDestinationTest, RegisterThreadLabelsSubsequentScopeEvents)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination destination;
    Profiler::RegisterDestination(destination);
    Profiler::SetEnabled(true);
    Profiler::RegisterThread("Render Thread");

    const auto scope = Profiler::BeginScope("Threaded Scope", __FUNCTION__);
    Profiler::EndScope(scope);

    ASSERT_EQ(destination.events.size(), 2u);
    EXPECT_EQ(destination.events[0].threadName, "Render Thread");
    EXPECT_EQ(destination.events[1].threadName, "Render Thread");
}

TEST_F(ProfilerDestinationTest, GpuScopeRoutesOnlyToGpuCapableDestinations)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination cpuOnly(ProfilerDestinationId::Tracy);
    RecordingProfilerDestination gpuCapable(ProfilerDestinationId::Timeline);
    gpuCapable.m_state.capabilities = ProfilerCapability_CPUScopes | ProfilerCapability_GPUScopes;

    Profiler::RegisterDestination(cpuOnly);
    Profiler::RegisterDestination(gpuCapable);
    Profiler::SetEnabled(true);
    Profiler::RegisterThread("RHI Thread");

    const auto scope = Profiler::BeginGpuScope(nullptr, "GPU Pass", __FUNCTION__);
    Profiler::EndGpuScope(scope);

    EXPECT_TRUE(cpuOnly.events.empty());
    ASSERT_EQ(gpuCapable.events.size(), 2u);
    EXPECT_EQ(gpuCapable.events[0].phase, "gpu-begin");
    EXPECT_EQ(gpuCapable.events[0].name, "GPU Pass");
    EXPECT_EQ(gpuCapable.events[0].threadName, "RHI Thread");
    EXPECT_EQ(gpuCapable.events[1].phase, "gpu-end");
    EXPECT_EQ(Profiler::GetSessionStats().acceptedEventCount, 1u);
    EXPECT_EQ(Profiler::GetSessionStats().droppedEventCount, 1u);
}

TEST_F(ProfilerDestinationTest, LateRegisteredTimelineDestinationReceivesLastGpuContext)
{
    using namespace NLS::Base::Profiling;

    int fakeDevice = 0;
    int fakeQueue = 0;
    ProfilerGpuContextEvent context;
    context.nativeDevice = &fakeDevice;
    context.nativeCommandQueues.push_back(&fakeQueue);
    context.frameLatency = 2u;

    Profiler::SetEnabled(true);
    Profiler::InitializeGpuContext(context);

    RecordingProfilerDestination timeline(ProfilerDestinationId::Timeline);
    timeline.m_state.capabilities = ProfilerCapability_CPUScopes | ProfilerCapability_EditorTimeline;
    Profiler::RegisterDestination(timeline);

    EXPECT_EQ(timeline.gpuContextInitializeCalls, 1);
    EXPECT_EQ(timeline.lastGpuContextQueueCount, 1u);
}

TEST_F(ProfilerDestinationTest, RegisteredTimelineDestinationReceivesGpuContextBeforeGpuScopesAreAvailable)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination timeline(ProfilerDestinationId::Timeline);
    timeline.m_state.capabilities = ProfilerCapability_CPUScopes | ProfilerCapability_EditorTimeline;
    Profiler::RegisterDestination(timeline);
    Profiler::SetEnabled(true);

    int fakeDevice = 0;
    int fakeQueue = 0;
    ProfilerGpuContextEvent context;
    context.nativeDevice = &fakeDevice;
    context.nativeCommandQueues.push_back(&fakeQueue);
    context.frameLatency = 2u;

    Profiler::InitializeGpuContext(context);

    EXPECT_EQ(timeline.gpuContextInitializeCalls, 1);
    EXPECT_EQ(timeline.lastGpuContextQueueCount, 1u);
}

TEST_F(ProfilerDestinationTest, ResetForTestingClearsDestinationsAndCounters)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination destination;
    Profiler::RegisterDestination(destination);
    Profiler::SetEnabled(true);
    const auto scope = Profiler::BeginScope("Before Reset", __FUNCTION__);
    Profiler::EndScope(scope);

    Profiler::ResetForTesting();
    EXPECT_FALSE(Profiler::IsEnabled());
    EXPECT_EQ(Profiler::GetDestinationCountForTesting(), 0u);
    EXPECT_EQ(Profiler::GetSessionStats().acceptedEventCount, 0u);
    EXPECT_EQ(Profiler::GetSessionStats().droppedEventCount, 0u);
}

TEST_F(ProfilerDestinationTest, DisabledProfilerDoesNotCallDestinations)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination destination;
    Profiler::RegisterDestination(destination);
    Profiler::SetEnabled(false);

    const auto scope = Profiler::BeginScope("Disabled Scope", __FUNCTION__);
    Profiler::EndScope(scope);

    EXPECT_TRUE(destination.events.empty());
    EXPECT_EQ(Profiler::GetSessionStats().acceptedEventCount, 0u);
    EXPECT_EQ(Profiler::GetSessionStats().droppedEventCount, 1u);
}

TEST_F(ProfilerDestinationTest, TracyProfilerReportsUnavailableWhenThirdPartyIsNotEnabled)
{
    using namespace NLS::Base::Profiling;

    TracyProfiler tracy;
    const auto state = tracy.GetState();

    EXPECT_EQ(state.id, ProfilerDestinationId::Tracy);
#if defined(NLS_ENABLE_TRACY) && !defined(NLS_TRACY_UNAVAILABLE)
    EXPECT_EQ(state.availability, ProfilerAvailability::Available);
    EXPECT_TRUE(state.enabled);
    EXPECT_TRUE(state.lastError.empty());
#else
    EXPECT_EQ(state.availability, ProfilerAvailability::Unavailable);
    EXPECT_FALSE(state.enabled);
    EXPECT_NE(state.lastError.find("Tracy"), std::string::npos);
#endif
}

TEST_F(ProfilerDestinationTest, TimelineSinkFormatsAvailabilityStates)
{
    using namespace NLS::Base::Profiling;

    EXPECT_STREQ(TimelineProfilerSink::FormatAvailability(ProfilerAvailability::Available), "Available");
    EXPECT_STREQ(TimelineProfilerSink::FormatAvailability(ProfilerAvailability::Disabled), "Disabled");
    EXPECT_STREQ(TimelineProfilerSink::FormatAvailability(ProfilerAvailability::Unavailable), "Unavailable");
    EXPECT_STREQ(TimelineProfilerSink::FormatAvailability(ProfilerAvailability::Unsupported), "Unsupported");
}

TEST_F(ProfilerDestinationTest, TimelineSinkReportsDisabledWhenBuildOptionIsOff)
{
    using namespace NLS::Base::Profiling;

    TimelineProfilerSink timeline;
    const auto state = timeline.GetState();

    EXPECT_EQ(state.id, ProfilerDestinationId::Timeline);
#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    EXPECT_EQ(state.availability, ProfilerAvailability::Available);
    EXPECT_TRUE(state.enabled);
    EXPECT_TRUE(state.lastError.empty());
    EXPECT_NE(state.capabilities & ProfilerCapability_CPUScopes, 0u);
    EXPECT_EQ(state.capabilities & ProfilerCapability_GPUScopes, 0u);
    EXPECT_NE(state.capabilities & ProfilerCapability_EditorTimeline, 0u);
#else
    EXPECT_EQ(state.availability, ProfilerAvailability::Disabled);
    EXPECT_FALSE(state.enabled);
    EXPECT_EQ(state.capabilities, ProfilerCapability_None);
    EXPECT_NE(state.lastError.find("TimelineProfiler"), std::string::npos);
    EXPECT_NE(state.lastError.find("NLS_ENABLE_TIMELINE_PROFILER"), std::string::npos);
#endif
}

TEST_F(ProfilerDestinationTest, TimelineSinkRecordsScopesWhenEnabled)
{
    using namespace NLS::Base::Profiling;

#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    TimelineProfilerSink timeline;
    const auto state = timeline.GetState();
    ASSERT_EQ(state.availability, ProfilerAvailability::Available);

    ProfilerScopeEvent event;
    event.name = "Timeline Enabled Scope";
    event.sourceFunction = __FUNCTION__;
    event.active = true;

    timeline.BeginScope(event);
    timeline.TickFrame();
    EXPECT_NO_FATAL_FAILURE(timeline.EndScope(event));

    EXPECT_GT(timeline.GetRecordedTrackCountForTesting(), 0u);
    EXPECT_EQ(timeline.GetTickFrameCountForTesting(), 1u);
#else
    SUCCEED() << "TimelineProfiler is disabled in this build.";
#endif
}

TEST_F(ProfilerDestinationTest, TimelineSinkEndScopeIgnoresUnmatchedEvent)
{
    using namespace NLS::Base::Profiling;

#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    TimelineProfilerSink timeline;
    const auto state = timeline.GetState();
    ASSERT_EQ(state.availability, ProfilerAvailability::Available);

    ProfilerScopeEvent event;
    event.name = "Long Running Timeline Scope";
    event.sourceFunction = __FUNCTION__;
    event.active = true;

    EXPECT_NO_FATAL_FAILURE(timeline.EndScope(event));
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}

TEST_F(ProfilerDestinationTest, UnavailableDestinationDoesNotBlockAvailableDestination)
{
    using namespace NLS::Base::Profiling;

    UnavailableProfilerDestination unavailable;
    RecordingProfilerDestination available;
    Profiler::RegisterDestination(unavailable);
    Profiler::RegisterDestination(available);
    Profiler::SetEnabled(true);

    const auto scope = Profiler::BeginScope("Fallback Scope", __FUNCTION__);
    Profiler::EndScope(scope);

    EXPECT_EQ(unavailable.beginCalls, 0);
    EXPECT_EQ(unavailable.endCalls, 0);
    ASSERT_EQ(available.events.size(), 2u);
    EXPECT_EQ(available.events[0].name, "Fallback Scope");
    EXPECT_EQ(Profiler::GetSessionStats().acceptedEventCount, 1u);
    EXPECT_EQ(Profiler::GetSessionStats().droppedEventCount, 1u);
}
