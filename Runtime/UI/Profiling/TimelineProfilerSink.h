#pragma once

#include "Profiling/Profiler.h"
#include "UIDef.h"

#ifndef NLS_ENABLE_TIMELINE_PROFILER
#define NLS_ENABLE_TIMELINE_PROFILER 0
#endif

namespace NLS::Base::Profiling
{
class NLS_UI_API TimelineProfilerSink final : public IProfilerDestination
{
public:
    TimelineProfilerSink();
    ~TimelineProfilerSink() override;

    TimelineProfilerSink(const TimelineProfilerSink&) = delete;
    TimelineProfilerSink& operator=(const TimelineProfilerSink&) = delete;
    TimelineProfilerSink(TimelineProfilerSink&&) = delete;
    TimelineProfilerSink& operator=(TimelineProfilerSink&&) = delete;

    void BeginScope(const ProfilerScopeEvent& event) override;
    void EndScope(const ProfilerScopeEvent& event) override;
    void BeginGpuScope(const ProfilerGpuScopeEvent& event) override;
    void EndGpuScope(const ProfilerGpuScopeEvent& event) override;
    void InitializeGpuContext(const ProfilerGpuContextEvent& event) override;
    void SubmitGpuCommandLists(const ProfilerGpuCommandListSubmitEvent& event) override;
    ProfilerDestinationState GetState() const override;

    bool PrepareTimelineUI();
    void TickFrame();
    void DrawTimeline();
    void SetRecordingEnabled(bool enabled);

    size_t GetRecordedTrackCountForTesting() const;
    size_t GetTickFrameCountForTesting() const;
    size_t GetSkippedScopeCountForTesting() const;

    static const char* FormatAvailability(ProfilerAvailability availability);

private:
#if NLS_ENABLE_TIMELINE_PROFILER
    bool m_recordingEnabled = false;
    bool m_frameStarted = false;
    bool m_gpuInitialized = false;
    size_t m_tickFrameCount = 0u;
    size_t m_skippedScopeCount = 0u;
#endif
};
}
