#pragma once

#include "Profiling/Profiler.h"
#include "UIDef.h"

namespace NLS::Base::Profiling
{
class NLS_UI_API TimelineProfilerSink final : public IProfilerDestination
{
public:
    TimelineProfilerSink();

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

    size_t GetRecordedTrackCountForTesting() const;
    size_t GetTickFrameCountForTesting() const;

    static const char* FormatAvailability(ProfilerAvailability availability);

private:
#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    bool m_frameStarted = false;
    bool m_gpuInitialized = false;
    size_t m_tickFrameCount = 0u;
#endif
};
}
