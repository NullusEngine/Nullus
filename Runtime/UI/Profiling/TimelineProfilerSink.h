#pragma once

#include <filesystem>
#include <fstream>

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
    bool BeginTraceExport(const std::filesystem::path& path);
    uint32_t UpdateTraceExport(uint32_t maxFrameCount);
    void EndTraceExport();
    bool IsTraceExportOpen() const;
    uint32_t GetTraceExportedFrameCount() const;

    size_t GetRecordedTrackCountForTesting() const;
    size_t GetTickFrameCountForTesting() const;
    size_t GetSkippedScopeCountForTesting() const;
    bool HasRecordedEventForTesting(const char* eventName) const;
    size_t CountRecordedEventsForTesting(const char* eventName, bool requireValid) const;
    uint32_t GetPendingGpuProfilerEventCountForTesting() const;
    uint32_t GetPendingGpuProfilerCommandListQueryCountForTesting() const;
    size_t GetSubmittedGpuProfilerReadbackCountForTesting() const;

    static const char* FormatAvailability(ProfilerAvailability availability);

private:
#if NLS_ENABLE_TIMELINE_PROFILER
    struct TraceExportState
    {
        std::ofstream stream;
        std::filesystem::path path;
        uint64_t baseTime = 0u;
        uint32_t lastExportedFrame = 0u;
        uint32_t exportedFrameCount = 0u;
    };

    bool m_recordingEnabled = false;
    bool m_frameStarted = false;
    bool m_gpuInitialized = false;
    bool m_gpuScopesAvailable = false;
    size_t m_tickFrameCount = 0u;
    size_t m_skippedScopeCount = 0u;
    TraceExportState m_traceExport;
#endif
};
}
