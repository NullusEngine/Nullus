#include "UI/Profiling/TimelineProfilerSink.h"
#include "UI/Profiling/TimelineProfilerLimits.h"

#if defined(NLS_ENABLE_TIMELINE_PROFILER)
#include <Profiler.h>

#if defined(_WIN32)
#include <d3d12.h>
#endif

namespace
{
thread_local uint32_t g_timelineScopeDepth = 0u;
thread_local uint32_t g_timelineSuppressedScopeDepth = 0u;

void EnsureTimelineProfilerInitialized()
{
    static const bool initialized = []
    {
        if (!gProfiler.IsInitialized())
            gProfiler.Initialize(NLS::UI::Profiling::kTimelineProfilerHistoryFrameCount);
        return true;
    }();
    (void)initialized;
}
}
#endif

namespace NLS::Base::Profiling
{
TimelineProfilerSink::TimelineProfilerSink()
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    EnsureTimelineProfilerInitialized();
#endif
}

void TimelineProfilerSink::BeginScope(const ProfilerScopeEvent& event)
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    if (!m_recordingEnabled)
        return;

    if (g_timelineSuppressedScopeDepth > 0u ||
        g_timelineScopeDepth >= NLS::UI::Profiling::kTimelineProfilerMaxCpuScopeDepth)
    {
        ++g_timelineSuppressedScopeDepth;
        ++m_skippedScopeCount;
        return;
    }

    if (!event.threadName.empty())
        gProfiler.RegisterCurrentThread(event.threadName.c_str());
    gProfiler.BeginEvent(event.name.c_str(), 0u, "", 0u);
    ++g_timelineScopeDepth;
#else
    (void)event;
#endif
}

void TimelineProfilerSink::EndScope(const ProfilerScopeEvent& event)
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    if (!m_recordingEnabled)
        return;

    (void)event;
    if (g_timelineSuppressedScopeDepth > 0u)
    {
        --g_timelineSuppressedScopeDepth;
        return;
    }

    if (g_timelineScopeDepth == 0u)
        return;

    gProfiler.EndEvent();
    --g_timelineScopeDepth;
#else
    (void)event;
#endif
}

void TimelineProfilerSink::BeginGpuScope(const ProfilerGpuScopeEvent& event)
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER) && defined(_WIN32)
    if (!m_recordingEnabled || !m_gpuInitialized || event.nativeCommandBuffer == nullptr)
        return;

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(event.nativeCommandBuffer);
    gGPUProfiler.BeginEvent(commandList, event.name.c_str(), 0u, "", 0u);
#else
    (void)event;
#endif
}

void TimelineProfilerSink::EndGpuScope(const ProfilerGpuScopeEvent& event)
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER) && defined(_WIN32)
    if (!m_recordingEnabled || !m_gpuInitialized || event.nativeCommandBuffer == nullptr)
        return;

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(event.nativeCommandBuffer);
    gGPUProfiler.EndEvent(commandList);
#else
    (void)event;
#endif
}

void TimelineProfilerSink::InitializeGpuContext(const ProfilerGpuContextEvent& event)
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER) && defined(_WIN32)
    if (m_gpuInitialized || event.nativeDevice == nullptr || event.nativeCommandQueues.empty())
        return;

    std::vector<ID3D12CommandQueue*> queues;
    queues.reserve(event.nativeCommandQueues.size());
    for (void* nativeQueue : event.nativeCommandQueues)
    {
        if (nativeQueue != nullptr)
            queues.push_back(static_cast<ID3D12CommandQueue*>(nativeQueue));
    }
    if (queues.empty())
        return;

    EnsureTimelineProfilerInitialized();
    auto* device = static_cast<ID3D12Device*>(event.nativeDevice);
    const uint32 frameLatency = event.frameLatency != 0u ? event.frameLatency : 2u;
    gGPUProfiler.Initialize(device, Span<ID3D12CommandQueue*>(queues.data(), queues.size()), frameLatency);
    m_gpuInitialized = true;
#else
    (void)event;
#endif
}

void TimelineProfilerSink::SubmitGpuCommandLists(const ProfilerGpuCommandListSubmitEvent& event)
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER) && defined(_WIN32)
    if (!m_recordingEnabled || !m_gpuInitialized || event.nativeCommandQueue == nullptr || event.nativeCommandLists.empty())
        return;

    std::vector<ID3D12CommandList*> commandLists;
    commandLists.reserve(event.nativeCommandLists.size());
    for (void* nativeCommandList : event.nativeCommandLists)
    {
        if (nativeCommandList != nullptr)
            commandLists.push_back(static_cast<ID3D12CommandList*>(nativeCommandList));
    }
    if (commandLists.empty())
        return;

    gGPUProfiler.ExecuteCommandLists(
        static_cast<ID3D12CommandQueue*>(event.nativeCommandQueue),
        Span<ID3D12CommandList*>(commandLists.data(), commandLists.size()));
#else
    (void)event;
#endif
}

bool TimelineProfilerSink::PrepareTimelineUI()
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    EnsureTimelineProfilerInitialized();
    return PrepareProfilerHUD();
#else
    return false;
#endif
}

void TimelineProfilerSink::TickFrame()
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    if (!m_recordingEnabled)
        return;

    g_timelineScopeDepth = 0u;
    g_timelineSuppressedScopeDepth = 0u;
    ++m_tickFrameCount;
    if (!m_frameStarted)
    {
        gProfiler.Tick();
        gGPUProfiler.Tick();
        m_frameStarted = true;
        return;
    }

    gProfiler.Tick();
    gGPUProfiler.Tick();
#endif
}

size_t TimelineProfilerSink::GetTickFrameCountForTesting() const
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    return m_tickFrameCount;
#else
    return 0u;
#endif
}

size_t TimelineProfilerSink::GetSkippedScopeCountForTesting() const
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    return m_skippedScopeCount;
#else
    return 0u;
#endif
}

void TimelineProfilerSink::DrawTimeline()
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    if (PrepareTimelineUI())
        DrawProfilerHUD();
#endif
}

void TimelineProfilerSink::SetRecordingEnabled(const bool enabled)
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    const bool wasRecordingEnabled = m_recordingEnabled;
    m_recordingEnabled = enabled;
    if (enabled && !wasRecordingEnabled)
        Profiler::ReplayGpuContextIfAvailable(*this);
    if (!enabled)
    {
        g_timelineScopeDepth = 0u;
        g_timelineSuppressedScopeDepth = 0u;
    }
#else
    (void)enabled;
#endif
}

size_t TimelineProfilerSink::GetRecordedTrackCountForTesting() const
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    return gProfiler.GetTracks().size();
#else
    return 0u;
#endif
}

ProfilerDestinationState TimelineProfilerSink::GetState() const
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    if (!m_recordingEnabled)
    {
        return {
            ProfilerDestinationId::Timeline,
            false,
            ProfilerAvailability::Disabled,
            ProfilerCapability_None,
            "TimelineProfiler destination is disabled because the Profiler panel is closed."
        };
    }

    return {
        ProfilerDestinationId::Timeline,
        true,
        ProfilerAvailability::Available,
        ProfilerCapability_CPUScopes |
            ProfilerCapability_EditorTimeline |
            (m_gpuInitialized ? ProfilerCapability_GPUScopes : ProfilerCapability_None),
        ""
    };
#else
    return {
        ProfilerDestinationId::Timeline,
        false,
        ProfilerAvailability::Disabled,
        ProfilerCapability_None,
        "TimelineProfiler destination is disabled. Reconfigure with NLS_ENABLE_TIMELINE_PROFILER=ON."
    };
#endif
}

const char* TimelineProfilerSink::FormatAvailability(const ProfilerAvailability availability)
{
    switch (availability)
    {
    case ProfilerAvailability::Available:
        return "Available";
    case ProfilerAvailability::Disabled:
        return "Disabled";
    case ProfilerAvailability::Unavailable:
        return "Unavailable";
    case ProfilerAvailability::Unsupported:
        return "Unsupported";
    default:
        return "Unknown";
    }
}
}
