#include "UI/Profiling/TimelineProfilerSink.h"

#if defined(NLS_ENABLE_TIMELINE_PROFILER)
#include <Profiler.h>

#if defined(_WIN32)
#include <d3d12.h>
#endif

namespace
{
void EnsureTimelineProfilerInitialized()
{
    static const bool initialized = []
    {
        if (!gProfiler.IsInitialized())
            gProfiler.Initialize(256u);
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
    if (!event.threadName.empty())
        gProfiler.RegisterCurrentThread(event.threadName.c_str());
    gProfiler.BeginEvent(event.name.c_str(), 0u, "", 0u);
#else
    (void)event;
#endif
}

void TimelineProfilerSink::EndScope(const ProfilerScopeEvent& event)
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    (void)event;
    gProfiler.EndEvent();
#else
    (void)event;
#endif
}

void TimelineProfilerSink::BeginGpuScope(const ProfilerGpuScopeEvent& event)
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER) && defined(_WIN32)
    if (!m_gpuInitialized || event.nativeCommandBuffer == nullptr)
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
    if (!m_gpuInitialized || event.nativeCommandBuffer == nullptr)
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
    if (!m_gpuInitialized || event.nativeCommandQueue == nullptr || event.nativeCommandLists.empty())
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

void TimelineProfilerSink::DrawTimeline()
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    if (PrepareTimelineUI())
        DrawProfilerHUD();
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
