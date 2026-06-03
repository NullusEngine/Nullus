#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Profiling/Profiler.h"
#include "UI/Profiling/TimelineProfilerSink.h"
#include "UI/Profiling/TimelineProfilerLimits.h"
#include "ImGuiExtensions/TimelineProfiler/Profiler.h"
#include "ImGuiExtensions/TimelineProfiler/ProfilerTraceCursor.h"
#include "Profiling/TracyProfiler.h"

namespace
{
using BaseProfiler = NLS::Base::Profiling::Profiler;

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
    BaseProfiler::RegisterDestination(destination);
    BaseProfiler::SetEnabled(true);

    const auto scope = BaseProfiler::BeginScope("Destination Scope", __FUNCTION__);
    BaseProfiler::EndScope(scope);

    ASSERT_EQ(destination.events.size(), 2u);
    EXPECT_EQ(destination.events[0].phase, "begin");
    EXPECT_EQ(destination.events[0].name, "Destination Scope");
    EXPECT_EQ(destination.events[1].phase, "end");
    EXPECT_EQ(destination.events[1].name, "Destination Scope");

    const auto stats = BaseProfiler::GetSessionStats();
    EXPECT_EQ(stats.acceptedEventCount, 1u);
    EXPECT_EQ(stats.droppedEventCount, 0u);
}

TEST_F(ProfilerDestinationTest, RoutesScopeToEveryAvailableDestination)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination first(ProfilerDestinationId::Tracy);
    RecordingProfilerDestination second(ProfilerDestinationId::Timeline);
    BaseProfiler::RegisterDestination(first);
    BaseProfiler::RegisterDestination(second);
    BaseProfiler::SetEnabled(true);

    const auto scope = BaseProfiler::BeginScope("Shared Scope", __FUNCTION__);
    BaseProfiler::EndScope(scope);

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
    BaseProfiler::RegisterDestination(destination);
    BaseProfiler::SetEnabled(true);

    EXPECT_EQ(BaseProfiler::GetDestinationCountForTesting(), 1u);

    BaseProfiler::UnregisterDestination(destination);

    EXPECT_EQ(BaseProfiler::GetDestinationCountForTesting(), 0u);

    const auto scope = BaseProfiler::BeginScope("After Unregister", __FUNCTION__);
    BaseProfiler::EndScope(scope);

    EXPECT_TRUE(destination.events.empty());
}

TEST_F(ProfilerDestinationTest, ScopeEndOnlyRoutesToDestinationsThatAcceptedBegin)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination earlyDestination;
    BaseProfiler::RegisterDestination(earlyDestination);
    BaseProfiler::SetEnabled(true);

    const auto scope = BaseProfiler::BeginScope("In Flight Scope", __FUNCTION__);

    RecordingProfilerDestination lateDestination;
    BaseProfiler::RegisterDestination(lateDestination);

    BaseProfiler::EndScope(scope);

    ASSERT_EQ(earlyDestination.events.size(), 2u);
    EXPECT_EQ(earlyDestination.events[0].phase, "begin");
    EXPECT_EQ(earlyDestination.events[1].phase, "end");
    EXPECT_TRUE(lateDestination.events.empty());
}

TEST_F(ProfilerDestinationTest, RegisterThreadLabelsSubsequentScopeEvents)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination destination;
    BaseProfiler::RegisterDestination(destination);
    BaseProfiler::SetEnabled(true);
    BaseProfiler::RegisterThread("Render Thread");

    const auto scope = BaseProfiler::BeginScope("Threaded Scope", __FUNCTION__);
    BaseProfiler::EndScope(scope);

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

    BaseProfiler::RegisterDestination(cpuOnly);
    BaseProfiler::RegisterDestination(gpuCapable);
    BaseProfiler::SetEnabled(true);
    BaseProfiler::RegisterThread("RHI Thread");

    const auto scope = BaseProfiler::BeginGpuScope(nullptr, "GPU Pass", __FUNCTION__);
    BaseProfiler::EndGpuScope(scope);

    EXPECT_TRUE(cpuOnly.events.empty());
    ASSERT_EQ(gpuCapable.events.size(), 2u);
    EXPECT_EQ(gpuCapable.events[0].phase, "gpu-begin");
    EXPECT_EQ(gpuCapable.events[0].name, "GPU Pass");
    EXPECT_EQ(gpuCapable.events[0].threadName, "RHI Thread");
    EXPECT_EQ(gpuCapable.events[1].phase, "gpu-end");
    EXPECT_EQ(BaseProfiler::GetSessionStats().acceptedEventCount, 1u);
    EXPECT_EQ(BaseProfiler::GetSessionStats().droppedEventCount, 1u);
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

    BaseProfiler::SetEnabled(true);
    BaseProfiler::InitializeGpuContext(context);

    RecordingProfilerDestination timeline(ProfilerDestinationId::Timeline);
    timeline.m_state.capabilities = ProfilerCapability_CPUScopes | ProfilerCapability_EditorTimeline;
    BaseProfiler::RegisterDestination(timeline);

    EXPECT_EQ(timeline.gpuContextInitializeCalls, 1);
    EXPECT_EQ(timeline.lastGpuContextQueueCount, 1u);
}

TEST_F(ProfilerDestinationTest, RegisteredTimelineDestinationReceivesGpuContextBeforeGpuScopesAreAvailable)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination timeline(ProfilerDestinationId::Timeline);
    timeline.m_state.capabilities = ProfilerCapability_CPUScopes | ProfilerCapability_EditorTimeline;
    BaseProfiler::RegisterDestination(timeline);
    BaseProfiler::SetEnabled(true);

    int fakeDevice = 0;
    int fakeQueue = 0;
    ProfilerGpuContextEvent context;
    context.nativeDevice = &fakeDevice;
    context.nativeCommandQueues.push_back(&fakeQueue);
    context.frameLatency = 2u;

    BaseProfiler::InitializeGpuContext(context);

    EXPECT_EQ(timeline.gpuContextInitializeCalls, 1);
    EXPECT_EQ(timeline.lastGpuContextQueueCount, 1u);
}

TEST_F(ProfilerDestinationTest, ReplaysGpuContextWhenDestinationBecomesAvailableAfterRegistration)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination timeline(ProfilerDestinationId::Timeline);
    timeline.m_state.enabled = false;
    timeline.m_state.availability = ProfilerAvailability::Disabled;
    timeline.m_state.capabilities = ProfilerCapability_None;
    BaseProfiler::RegisterDestination(timeline);
    BaseProfiler::SetEnabled(true);

    int fakeDevice = 0;
    int fakeQueue = 0;
    ProfilerGpuContextEvent context;
    context.nativeDevice = &fakeDevice;
    context.nativeCommandQueues.push_back(&fakeQueue);
    context.frameLatency = 2u;
    BaseProfiler::InitializeGpuContext(context);

    EXPECT_EQ(timeline.gpuContextInitializeCalls, 0);

    timeline.m_state.enabled = true;
    timeline.m_state.availability = ProfilerAvailability::Available;
    timeline.m_state.capabilities = ProfilerCapability_CPUScopes | ProfilerCapability_EditorTimeline;
    BaseProfiler::ReplayGpuContextIfAvailable(timeline);

    EXPECT_EQ(timeline.gpuContextInitializeCalls, 1);
    EXPECT_EQ(timeline.lastGpuContextQueueCount, 1u);
}

TEST_F(ProfilerDestinationTest, ResetForTestingClearsDestinationsAndCounters)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination destination;
    BaseProfiler::RegisterDestination(destination);
    BaseProfiler::SetEnabled(true);
    const auto scope = BaseProfiler::BeginScope("Before Reset", __FUNCTION__);
    BaseProfiler::EndScope(scope);

    BaseProfiler::ResetForTesting();
    EXPECT_FALSE(BaseProfiler::IsEnabled());
    EXPECT_EQ(BaseProfiler::GetDestinationCountForTesting(), 0u);
    EXPECT_EQ(BaseProfiler::GetSessionStats().acceptedEventCount, 0u);
    EXPECT_EQ(BaseProfiler::GetSessionStats().droppedEventCount, 0u);
}

TEST_F(ProfilerDestinationTest, DisabledProfilerDoesNotCallDestinations)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination destination;
    BaseProfiler::RegisterDestination(destination);
    BaseProfiler::SetEnabled(false);

    const auto scope = BaseProfiler::BeginScope("Disabled Scope", __FUNCTION__);
    BaseProfiler::EndScope(scope);

    EXPECT_TRUE(destination.events.empty());
    EXPECT_EQ(BaseProfiler::GetSessionStats().acceptedEventCount, 0u);
    EXPECT_EQ(BaseProfiler::GetSessionStats().droppedEventCount, 0u);
}

TEST_F(ProfilerDestinationTest, TracyProfilerReportsUnavailableWhenThirdPartyIsNotEnabled)
{
    using namespace NLS::Base::Profiling;

    TracyProfiler tracy;
    const auto state = tracy.GetState();

    EXPECT_EQ(state.id, ProfilerDestinationId::Tracy);
#if NLS_ENABLE_TRACY && !defined(NLS_TRACY_UNAVAILABLE)
    if (TracyProfiler::IsConnected())
    {
        EXPECT_EQ(state.availability, ProfilerAvailability::Available);
        EXPECT_TRUE(state.enabled);
        EXPECT_TRUE(state.lastError.empty());
    }
    else
    {
        EXPECT_EQ(state.availability, ProfilerAvailability::Disabled);
        EXPECT_FALSE(state.enabled);
        EXPECT_NE(state.lastError.find("not connected"), std::string::npos);
    }
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
#if NLS_ENABLE_TIMELINE_PROFILER
    EXPECT_EQ(state.availability, ProfilerAvailability::Disabled);
    EXPECT_FALSE(state.enabled);
    EXPECT_EQ(state.capabilities, ProfilerCapability_None);
    EXPECT_NE(state.lastError.find("Profiler panel is closed"), std::string::npos);
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

#if NLS_ENABLE_TIMELINE_PROFILER
    TimelineProfilerSink timeline;
    timeline.SetRecordingEnabled(true);
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

#if NLS_ENABLE_TIMELINE_PROFILER
    TimelineProfilerSink timeline;
    timeline.SetRecordingEnabled(true);
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

TEST_F(ProfilerDestinationTest, TimelineSinkSuppressesScopesBeyondInternalStackLimit)
{
    using namespace NLS::Base::Profiling;

#if NLS_ENABLE_TIMELINE_PROFILER
    TimelineProfilerSink timeline;
    timeline.SetRecordingEnabled(true);
    ASSERT_EQ(timeline.GetState().availability, ProfilerAvailability::Available);

    ProfilerScopeEvent event;
    event.name = "Deep Timeline Scope";
    event.sourceFunction = __FUNCTION__;
    event.active = true;

    for (size_t i = 0u; i < 40u; ++i)
        timeline.BeginScope(event);

    EXPECT_GT(timeline.GetSkippedScopeCountForTesting(), 0u);

    for (size_t i = 0u; i < 40u; ++i)
        EXPECT_NO_FATAL_FAILURE(timeline.EndScope(event));
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}

TEST_F(ProfilerDestinationTest, TimelineSinkKeepsEditorPanelDepthScopes)
{
    using namespace NLS::Base::Profiling;

#if NLS_ENABLE_TIMELINE_PROFILER
    TimelineProfilerSink timeline;
    timeline.SetRecordingEnabled(true);
    ASSERT_EQ(timeline.GetState().availability, ProfilerAvailability::Available);

    std::vector<ProfilerScopeEvent> events;
    events.reserve(NLS::UI::Profiling::kTimelineProfilerMaxCpuScopeDepth);
    for (size_t index = 0u; index < NLS::UI::Profiling::kTimelineProfilerMaxCpuScopeDepth; ++index)
    {
        ProfilerScopeEvent event;
        event.name = "Nested Editor UI Scope " + std::to_string(index);
        event.sourceFunction = __FUNCTION__;
        event.active = true;
        events.push_back(std::move(event));
        timeline.BeginScope(events.back());
    }

    EXPECT_EQ(timeline.GetSkippedScopeCountForTesting(), 0u);

    for (auto it = events.rbegin(); it != events.rend(); ++it)
        EXPECT_NO_FATAL_FAILURE(timeline.EndScope(*it));
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}

TEST_F(ProfilerDestinationTest, SelectionOutlineMaskAggregateScopesRemainExportableAtSceneViewDepth)
{
#if NLS_ENABLE_TIMELINE_PROFILER
    using namespace NLS::Base::Profiling;

    TimelineProfilerSink timeline;
    timeline.SetRecordingEnabled(true);
    ASSERT_EQ(timeline.GetState().availability, ProfilerAvailability::Available);

    std::vector<ProfilerScopeEvent> parentScopes;
    parentScopes.reserve(NLS::UI::Profiling::kTimelineProfilerMaxCpuScopeDepth - 1u);
    for (size_t index = 0u; index + 1u < NLS::UI::Profiling::kTimelineProfilerMaxCpuScopeDepth; ++index)
    {
        ProfilerScopeEvent event;
        event.name = "Scene View Parent Scope " + std::to_string(index);
        event.sourceFunction = __FUNCTION__;
        event.active = true;
        parentScopes.push_back(std::move(event));
        timeline.BeginScope(parentScopes.back());
    }

    const std::array<const char*, 2> selectionScopeNames = {
        "SelectionOutlineMask::CaptureMask",
        "SelectionOutlineMask::Composite"
    };

    for (const char* name : selectionScopeNames)
    {
        ProfilerScopeEvent event;
        event.name = name;
        event.sourceFunction = __FUNCTION__;
        event.active = true;
        timeline.BeginScope(event);
        EXPECT_NO_FATAL_FAILURE(timeline.EndScope(event));
    }

    EXPECT_EQ(timeline.GetSkippedScopeCountForTesting(), 0u);

    for (auto it = parentScopes.rbegin(); it != parentScopes.rend(); ++it)
        EXPECT_NO_FATAL_FAILURE(timeline.EndScope(*it));
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}

TEST_F(ProfilerDestinationTest, TimelineTraceExporterWritesEachFrameOnce)
{
#if NLS_ENABLE_TIMELINE_PROFILER
    using NLS::UI::TimelineProfilerDetail::ResolveTraceFrameExportRange;

    std::uint32_t lastExportedFrame = 0u;

    auto exportRange = ResolveTraceFrameExportRange({1u, 4u}, lastExportedFrame);
    EXPECT_EQ(exportRange.Begin, 1u);
    EXPECT_EQ(exportRange.End, 4u);
    lastExportedFrame = exportRange.End - 1u;

    exportRange = ResolveTraceFrameExportRange({1u, 4u}, lastExportedFrame);
    EXPECT_EQ(exportRange.Begin, 0u);
    EXPECT_EQ(exportRange.End, 0u);
    EXPECT_EQ(lastExportedFrame, 3u);

    exportRange = ResolveTraceFrameExportRange({1u, 6u}, lastExportedFrame);
    EXPECT_EQ(exportRange.Begin, 4u);
    EXPECT_EQ(exportRange.End, 6u);
    lastExportedFrame = exportRange.End - 1u;

    exportRange = ResolveTraceFrameExportRange({10u, 13u}, lastExportedFrame);
    EXPECT_EQ(exportRange.Begin, 10u);
    EXPECT_EQ(exportRange.End, 13u);
    EXPECT_EQ(lastExportedFrame, 9u);

    const auto profilerWindowPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/UI/ImGuiExtensions/TimelineProfiler/ProfilerWindow.cpp";

    std::ifstream stream(profilerWindowPath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("MaxDepth = static_cast<int>(NLS::UI::Profiling::kTimelineProfilerMaxCpuScopeDepth)"), std::string::npos);
    EXPECT_NE(source.find("ImGui::SliderInt(\"Depth\", &style.MaxDepth, 1, static_cast<int>(NLS::UI::Profiling::kTimelineProfilerMaxCpuScopeDepth))"), std::string::npos);
    EXPECT_NE(source.find("LastExportedFrame"), std::string::npos);
    EXPECT_NE(source.find("ResolveTraceFrameExportRange("), std::string::npos);
    EXPECT_NE(source.find("{ cpuRange.Begin, cpuRange.End }"), std::string::npos);
    EXPECT_NE(source.find("context.LastExportedFrame = cpuRange.End > 0 ? cpuRange.End - 1 : 0;"), std::string::npos);
    EXPECT_NE(source.find("context.LastExportedFrame = frameIndex;"), std::string::npos);
    EXPECT_NE(source.find("BuildTraceMetadataEventJson("), std::string::npos);
    EXPECT_NE(source.find("\"thread_name\""), std::string::npos);
    EXPECT_EQ(source.find("Sprintf(\"{\\\"name\\\":\\\"thread_name\\\""), std::string::npos);
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}

TEST_F(ProfilerDestinationTest, TimelineTraceExporterSkipsIncompleteAndNonPositiveDurationEvents)
{
#if NLS_ENABLE_TIMELINE_PROFILER
    using NLS::UI::TimelineProfilerDetail::BuildTraceMetadataEventJson;
    using NLS::UI::TimelineProfilerDetail::BuildTraceDurationEventJson;
    using NLS::UI::TimelineProfilerDetail::ShouldExportTraceDurationEvent;

    EXPECT_EQ(
        BuildTraceMetadataEventJson("thread_name", 0u, 7u, "Quote\"Slash\\Line\nTab\t"),
        "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":0,\"tid\":7,\"args\":{\"name\":\"Quote\\\"Slash\\\\Line\\nTab\\t\"}}");
    EXPECT_EQ(
        BuildTraceMetadataEventJson("process_name", 0u, std::nullopt, "Track"),
        "{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":0,\"args\":{\"name\":\"Track\"}}");

    EXPECT_FALSE(ShouldExportTraceDurationEvent(0u, 10u));
    EXPECT_FALSE(ShouldExportTraceDurationEvent(10u, 0u));
    EXPECT_FALSE(ShouldExportTraceDurationEvent(10u, 10u));
    EXPECT_FALSE(ShouldExportTraceDurationEvent(20u, 10u));
    EXPECT_TRUE(ShouldExportTraceDurationEvent(10u, 20u));

    EXPECT_EQ(BuildTraceDurationEventJson(7u, 0u, 20u, 10u, 0.5, "bad"), std::nullopt);
    EXPECT_EQ(BuildTraceDurationEventJson(7u, 20u, 20u, 10u, 0.5, "bad"), std::nullopt);
    EXPECT_EQ(BuildTraceDurationEventJson(7u, 30u, 20u, 10u, 0.5, "bad"), std::nullopt);

    const auto exported = BuildTraceDurationEventJson(7u, 20u, 24u, 10u, 0.5, "Valid Scope");
    ASSERT_TRUE(exported.has_value());
    EXPECT_NE(exported->find("\"tid\":7"), std::string::npos);
    EXPECT_NE(exported->find("\"ts\":5000"), std::string::npos);
    EXPECT_NE(exported->find("\"dur\":2000"), std::string::npos);
    EXPECT_NE(exported->find("\"name\":\"Valid Scope\""), std::string::npos);
    EXPECT_EQ(exported->find("\"dur\":0"), std::string::npos);

    const auto tinyDuration = BuildTraceDurationEventJson(7u, 20u, 21u, 10u, 0.0001, "Tiny Scope");
    ASSERT_TRUE(tinyDuration.has_value());
    EXPECT_NE(tinyDuration->find("\"dur\":1"), std::string::npos);
    EXPECT_EQ(tinyDuration->find("\"dur\":0"), std::string::npos);

    const auto escapedName = BuildTraceDurationEventJson(7u, 20u, 24u, 10u, 0.5, "Quote\"Slash\\Line\nTab\t");
    ASSERT_TRUE(escapedName.has_value());
    EXPECT_NE(escapedName->find("\"name\":\"Quote\\\"Slash\\\\Line\\nTab\\t\""), std::string::npos);
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}

TEST_F(ProfilerDestinationTest, TimelineTraceExporterCanFilterEditorUiNoise)
{
    using NLS::UI::TimelineProfilerDetail::BuildTraceDurationEventJson;
    using NLS::UI::TimelineProfilerDetail::IsEditorUiTraceEventName;

    EXPECT_TRUE(IsEditorUiTraceEventName("Editor::RenderEditorUI"));
    EXPECT_TRUE(IsEditorUiTraceEventName("Canvas::DrawPanels"));
    EXPECT_TRUE(IsEditorUiTraceEventName("Panel::Draw:Profiler"));
    EXPECT_TRUE(IsEditorUiTraceEventName("Panel::Draw:Material Editor"));
    EXPECT_TRUE(IsEditorUiTraceEventName("Panel::Draw:Scene View"));
    EXPECT_TRUE(IsEditorUiTraceEventName("DX12UIBridge::RenderDrawData"));
    EXPECT_TRUE(IsEditorUiTraceEventName("NLS::Render::RHI::DX12UIBridge::RenderDrawData"));
    EXPECT_FALSE(IsEditorUiTraceEventName("NLS::Engine::Rendering::BaseSceneRenderer::CaptureThreadedPreparedDraw"));

    EXPECT_EQ(
        BuildTraceDurationEventJson(7u, 20u, 24u, 10u, 0.5, "Panel::Draw:Profiler", false),
        std::nullopt);
    EXPECT_EQ(
        BuildTraceDurationEventJson(7u, 20u, 24u, 10u, 0.5, "DX12UIBridge::RenderDrawData", false),
        std::nullopt);
    EXPECT_EQ(
        BuildTraceDurationEventJson(
            7u,
            20u,
            24u,
            10u,
            0.5,
            "NLS::Render::RHI::DX12UIBridge::RenderDrawData",
            false),
        std::nullopt);

    const auto sceneDraw = BuildTraceDurationEventJson(
        7u,
        20u,
        24u,
        10u,
        0.5,
        "NLS::Engine::Rendering::BaseSceneRenderer::CaptureThreadedPreparedDraw",
        false);
    ASSERT_TRUE(sceneDraw.has_value());
    EXPECT_NE(sceneDraw->find("CaptureThreadedPreparedDraw"), std::string::npos);
}

TEST_F(ProfilerDestinationTest, TimelineTraceExporterKeepsEventNamesOwnedUntilExport)
{
#if WITH_PROFILING
    ProfilerEvent event{};
    {
        std::string transientName = "NLS::Render::RHI::DX12UIBridge::RenderDrawData";
        event.SetName(transientName.c_str());
    }

    const auto exported = NLS::UI::TimelineProfilerDetail::BuildTraceDurationEventJson(
        4u,
        20u,
        24u,
        10u,
        0.5,
        event.GetName());

    ASSERT_TRUE(exported.has_value());
    EXPECT_NE(
        exported->find("\"name\":\"NLS::Render::RHI::DX12UIBridge::RenderDrawData\""),
        std::string::npos);
    EXPECT_EQ(exported->find("\"name\":\"rawData\""), std::string::npos);
    EXPECT_EQ(exported->find("\"name\":\"l\""), std::string::npos);
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}

TEST_F(ProfilerDestinationTest, TimelineProfilerEventsOwnNamesInsteadOfAllocatorPointers)
{
    const auto profilerHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.h";
    const auto profilerSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.cpp";
    const auto profilerWindowPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/UI/ImGuiExtensions/TimelineProfiler/ProfilerWindow.cpp";

    std::ifstream headerStream(profilerHeaderPath, std::ios::binary);
    const std::string header{
        std::istreambuf_iterator<char>(headerStream),
        std::istreambuf_iterator<char>()};
    std::ifstream sourceStream(profilerSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(sourceStream),
        std::istreambuf_iterator<char>()};
    std::ifstream windowStream(profilerWindowPath, std::ios::binary);
    const std::string window{
        std::istreambuf_iterator<char>(windowStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(window.empty());

    const auto eventStructBegin = header.find("struct ProfilerEvent");
    ASSERT_NE(eventStructBegin, std::string::npos);
    const auto eventStructEnd = header.find("// Data for a single frame", eventStructBegin);
    ASSERT_NE(eventStructEnd, std::string::npos);
    const auto eventStruct = header.substr(eventStructBegin, eventStructEnd - eventStructBegin);

    EXPECT_NE(header.find("char\t\tName[MaxNameLength + 1u]{}"), std::string::npos);
    EXPECT_NE(header.find("const char* GetName() const"), std::string::npos);
    EXPECT_NE(header.find("void SetName(const char* pName)"), std::string::npos);
    EXPECT_EQ(eventStruct.find("pName\t"), std::string::npos);
    EXPECT_EQ(eventStruct.find("pName;"), std::string::npos);
    EXPECT_NE(source.find("event.SetName(pName);"), std::string::npos);
    EXPECT_NE(source.find("newEvent.SetName(pName);"), std::string::npos);
    EXPECT_NE(source.find("event.SetName(\"Present\");"), std::string::npos);
    EXPECT_EQ(source.find(".pName"), std::string::npos);
    EXPECT_EQ(window.find(".pName"), std::string::npos);
    EXPECT_NE(window.find("event.GetName()"), std::string::npos);
}

TEST_F(ProfilerDestinationTest, TimelineTraceRecordingContinuesDrawingTimelineWhileExporting)
{
#if NLS_ENABLE_TIMELINE_PROFILER
    const auto profilerWindowPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/UI/ImGuiExtensions/TimelineProfiler/ProfilerWindow.cpp";

    std::ifstream stream(profilerWindowPath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    EXPECT_EQ(source.find("DrawProfilerTraceControls(traceContext"), std::string::npos);
    EXPECT_EQ(source.find("IsTraceRecording(traceContext)"), std::string::npos);
    EXPECT_EQ(source.find("Trace recording active; timeline drawing is paused."), std::string::npos);
    EXPECT_EQ(source.find("return; // Keep trace capture from profiling the profiler timeline UI."), std::string::npos);

    const auto drawBegin = source.find("static void DrawProfilerTimeline(");
    ASSERT_NE(drawBegin, std::string::npos);
    const auto drawEnd = source.find("void DrawProfilerHUD()", drawBegin);
    ASSERT_NE(drawEnd, std::string::npos);
    const auto drawCode = source.substr(drawBegin, drawEnd - drawBegin);
    const auto updateTrace = drawCode.find("UpdateTrace(traceContext);");
    ASSERT_NE(updateTrace, std::string::npos);
    const auto sizeActual = drawCode.find("ImVec2 sizeActual", updateTrace);
    ASSERT_NE(sizeActual, std::string::npos);
    EXPECT_EQ(drawCode.find("return;", updateTrace), std::string::npos);
    EXPECT_LT(updateTrace, sizeActual);
    EXPECT_NE(drawCode.find("if (!traceContext.TraceStream.is_open())"), std::string::npos);
    EXPECT_NE(drawCode.find("BeginTrace(pTracePath, traceContext)"), std::string::npos);
    EXPECT_NE(drawCode.find("EndTrace(traceContext)"), std::string::npos);
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}

TEST_F(ProfilerDestinationTest, UnavailableDestinationDoesNotBlockAvailableDestination)
{
    using namespace NLS::Base::Profiling;

    UnavailableProfilerDestination unavailable;
    RecordingProfilerDestination available;
    BaseProfiler::RegisterDestination(unavailable);
    BaseProfiler::RegisterDestination(available);
    BaseProfiler::SetEnabled(true);

    const auto scope = BaseProfiler::BeginScope("Fallback Scope", __FUNCTION__);
    BaseProfiler::EndScope(scope);

    EXPECT_EQ(unavailable.beginCalls, 0);
    EXPECT_EQ(unavailable.endCalls, 0);
    ASSERT_EQ(available.events.size(), 2u);
    EXPECT_EQ(available.events[0].name, "Fallback Scope");
    EXPECT_EQ(BaseProfiler::GetSessionStats().acceptedEventCount, 1u);
    EXPECT_EQ(BaseProfiler::GetSessionStats().droppedEventCount, 1u);
}
