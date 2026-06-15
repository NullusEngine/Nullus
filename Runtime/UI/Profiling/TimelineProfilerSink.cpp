#include "UI/Profiling/TimelineProfilerSink.h"
#include "UI/Profiling/TimelineProfilerLimits.h"
#include "ProfilerTraceCursor.h"

#if NLS_ENABLE_TIMELINE_PROFILER
#include <Profiler.h>

#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#include <d3d12.h>
#include <windows.h>
#endif

namespace
{
thread_local uint32_t g_timelineScopeDepth = 0u;
thread_local uint32_t g_timelineSuppressedScopeDepth = 0u;
thread_local uint32_t g_timelineFlushedScopeDepth = 0u;
std::mutex g_timelineGpuProfilerMutex;
uint32_t g_timelineGpuProfilerOwnerCount = 0u;
bool g_timelineGpuProfilerInitialized = false;
#if defined(_WIN32)
ID3D12Device* g_timelineGpuProfilerDevice = nullptr;
std::vector<ID3D12CommandQueue*> g_timelineGpuProfilerQueues;
uint32_t g_timelineGpuProfilerFrameLatency = 0u;
#endif

#if defined(_WIN32)
void ClearTimelineGpuProfilerContextLocked()
{
    g_timelineGpuProfilerInitialized = false;
    g_timelineGpuProfilerDevice = nullptr;
    g_timelineGpuProfilerQueues.clear();
    g_timelineGpuProfilerFrameLatency = 0u;
}
#endif

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

void FlushOpenTimelineCpuScopes()
{
    const uint32_t flushedScopeDepth = g_timelineScopeDepth;
    while (g_timelineScopeDepth > 0u)
    {
        gProfiler.EndEvent();
        --g_timelineScopeDepth;
    }
    g_timelineFlushedScopeDepth += flushedScopeDepth;
}

void ShutdownTimelineGpuProfilerForSink(bool& gpuInitialized)
{
    if (!gpuInitialized)
        return;

    std::lock_guard lock(g_timelineGpuProfilerMutex);
    gAssert(g_timelineGpuProfilerOwnerCount > 0u, "Timeline GPU profiler owner count underflow.");
    const bool lastOwner = g_timelineGpuProfilerOwnerCount == 1u;
    if (lastOwner && g_timelineGpuProfilerInitialized)
    {
        if (gGPUProfiler.Shutdown())
        {
#if defined(_WIN32)
            ClearTimelineGpuProfilerContextLocked();
#endif
        }
    }

    if (g_timelineGpuProfilerOwnerCount > 0u)
        --g_timelineGpuProfilerOwnerCount;
    gpuInitialized = false;
}

void ReleaseTimelineGpuProfilerOwnershipForSink(bool& gpuInitialized)
{
    if (!gpuInitialized)
        return;

    gAssert(g_timelineGpuProfilerOwnerCount > 0u, "Timeline GPU profiler owner count underflow.");
    if (g_timelineGpuProfilerOwnerCount > 0u)
        --g_timelineGpuProfilerOwnerCount;
    gpuInitialized = false;
}

void ReleaseTimelineGpuProfilerOwnershipForSinkLocked(bool& gpuInitialized)
{
    std::lock_guard lock(g_timelineGpuProfilerMutex);
    ReleaseTimelineGpuProfilerOwnershipForSink(gpuInitialized);
}

#if defined(_WIN32)
bool IsTimelineGpuProfilerContextCurrent(
    ID3D12Device* device,
    const std::vector<ID3D12CommandQueue*>& queues,
    const uint32_t frameLatency)
{
    return g_timelineGpuProfilerInitialized &&
        g_timelineGpuProfilerDevice == device &&
        g_timelineGpuProfilerFrameLatency == frameLatency &&
        g_timelineGpuProfilerQueues == queues;
}

bool InitializeTimelineGpuProfilerLocked(
    ID3D12Device* device,
    std::vector<ID3D12CommandQueue*>& queues,
    const uint32_t frameLatency)
{
    if (!IsTimelineGpuProfilerContextCurrent(device, queues, frameLatency))
    {
        if (g_timelineGpuProfilerInitialized && !gGPUProfiler.Shutdown())
            return false;

        if (g_timelineGpuProfilerInitialized)
            ClearTimelineGpuProfilerContextLocked();

        gGPUProfiler.Initialize(
            device,
            Span<ID3D12CommandQueue*>(queues.data(), queues.size()),
            frameLatency);
        g_timelineGpuProfilerInitialized = true;
        g_timelineGpuProfilerDevice = device;
        g_timelineGpuProfilerQueues = queues;
        g_timelineGpuProfilerFrameLatency = frameLatency;
    }
    return true;
}
#endif
}
#endif

namespace NLS::Base::Profiling
{
namespace
{
uint64_t QueryTimelineTraceCounter()
{
#if NLS_ENABLE_TIMELINE_PROFILER && defined(_WIN32)
    uint64_t value = 0u;
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&value));
    return value;
#else
    return 0u;
#endif
}

double QueryTimelineTraceTicksToMs()
{
#if NLS_ENABLE_TIMELINE_PROFILER && defined(_WIN32)
    uint64_t frequency = 0u;
    QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&frequency));
    return frequency != 0u ? 1000.0 / static_cast<double>(frequency) : 0.0;
#else
    return 0.0;
#endif
}
}

TimelineProfilerSink::TimelineProfilerSink()
{
#if NLS_ENABLE_TIMELINE_PROFILER
    EnsureTimelineProfilerInitialized();
#endif
}

TimelineProfilerSink::~TimelineProfilerSink()
{
#if NLS_ENABLE_TIMELINE_PROFILER
    EndTraceExport();
    ShutdownTimelineGpuProfilerForSink(m_gpuInitialized);
    ReleaseTimelineGpuProfilerOwnershipForSinkLocked(m_gpuInitialized);
    m_gpuScopesAvailable = false;
#endif
}

void TimelineProfilerSink::BeginScope(const ProfilerScopeEvent& event)
{
#if NLS_ENABLE_TIMELINE_PROFILER
    if (!m_recordingEnabled)
        return;

    const bool isAmbiguousWithFlushedParent =
        g_timelineFlushedScopeDepth > 0u && event.depth < g_timelineFlushedScopeDepth;
    if (g_timelineSuppressedScopeDepth > 0u ||
        isAmbiguousWithFlushedParent ||
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
#if NLS_ENABLE_TIMELINE_PROFILER
    if (!m_recordingEnabled)
        return;

    (void)event;
    if (g_timelineSuppressedScopeDepth > 0u)
    {
        --g_timelineSuppressedScopeDepth;
        return;
    }

    if (g_timelineFlushedScopeDepth > 0u && event.depth < g_timelineFlushedScopeDepth)
    {
        --g_timelineFlushedScopeDepth;
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
#if NLS_ENABLE_TIMELINE_PROFILER && defined(_WIN32)
    if (!m_recordingEnabled || !m_gpuScopesAvailable || event.nativeCommandBuffer == nullptr)
        return;

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(event.nativeCommandBuffer);
    std::lock_guard lock(g_timelineGpuProfilerMutex);
    gGPUProfiler.BeginEvent(commandList, event.name.c_str(), 0u, "", 0u);
#else
    (void)event;
#endif
}

void TimelineProfilerSink::EndGpuScope(const ProfilerGpuScopeEvent& event)
{
#if NLS_ENABLE_TIMELINE_PROFILER && defined(_WIN32)
    if (!m_recordingEnabled || !m_gpuScopesAvailable || event.nativeCommandBuffer == nullptr)
        return;

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(event.nativeCommandBuffer);
    std::lock_guard lock(g_timelineGpuProfilerMutex);
    gGPUProfiler.EndEvent(commandList);
#else
    (void)event;
#endif
}

void TimelineProfilerSink::InitializeGpuContext(const ProfilerGpuContextEvent& event)
{
#if NLS_ENABLE_TIMELINE_PROFILER && defined(_WIN32)
    if (event.nativeDevice == nullptr || event.nativeCommandQueues.empty())
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
    {
        std::lock_guard lock(g_timelineGpuProfilerMutex);
        const bool wasOwner = m_gpuInitialized;
        if (!InitializeTimelineGpuProfilerLocked(device, queues, frameLatency))
        {
            ReleaseTimelineGpuProfilerOwnershipForSink(m_gpuInitialized);
            m_gpuScopesAvailable = false;
            return;
        }

        m_gpuScopesAvailable = gGPUProfiler.HasProfileableQueues();

        if (!wasOwner)
        {
            ++g_timelineGpuProfilerOwnerCount;
            m_gpuInitialized = true;
        }
    }
#else
    (void)event;
#endif
}

void TimelineProfilerSink::SubmitGpuCommandLists(const ProfilerGpuCommandListSubmitEvent& event)
{
#if NLS_ENABLE_TIMELINE_PROFILER && defined(_WIN32)
    if (!m_recordingEnabled || !m_gpuScopesAvailable || event.nativeCommandQueue == nullptr || event.nativeCommandLists.empty())
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

    std::lock_guard lock(g_timelineGpuProfilerMutex);
    gGPUProfiler.ExecuteCommandLists(
        static_cast<ID3D12CommandQueue*>(event.nativeCommandQueue),
        Span<ID3D12CommandList*>(commandLists.data(), commandLists.size()));
#else
    (void)event;
#endif
}

bool TimelineProfilerSink::PrepareTimelineUI()
{
#if NLS_ENABLE_TIMELINE_PROFILER
    EnsureTimelineProfilerInitialized();
    return PrepareProfilerHUD();
#else
    return false;
#endif
}

void TimelineProfilerSink::TickFrame()
{
#if NLS_ENABLE_TIMELINE_PROFILER
    if (!m_recordingEnabled)
        return;

    FlushOpenTimelineCpuScopes();
    g_timelineScopeDepth = 0u;
    ++m_tickFrameCount;
    {
        gProfiler.Tick();
        NLS_PROFILE_NAMED_SCOPE("TimelineProfiler::TickFrame");
        std::lock_guard lock(g_timelineGpuProfilerMutex);
        gGPUProfiler.Tick();
    }
    if (!m_frameStarted)
    {
        m_frameStarted = true;
        return;
    }
#endif
}

size_t TimelineProfilerSink::GetTickFrameCountForTesting() const
{
#if NLS_ENABLE_TIMELINE_PROFILER
    return m_tickFrameCount;
#else
    return 0u;
#endif
}

size_t TimelineProfilerSink::GetSkippedScopeCountForTesting() const
{
#if NLS_ENABLE_TIMELINE_PROFILER
    return m_skippedScopeCount;
#else
    return 0u;
#endif
}

bool TimelineProfilerSink::HasRecordedEventForTesting(const char* eventName) const
{
    return CountRecordedEventsForTesting(eventName, true) != 0u;
}

size_t TimelineProfilerSink::CountRecordedEventsForTesting(const char* eventName, bool requireValid) const
{
#if NLS_ENABLE_TIMELINE_PROFILER
    if (eventName == nullptr)
        return 0u;

    size_t count = 0u;
    for (const auto& track : gProfiler.GetTracks())
    {
        for (const auto& frameEvents : track.Events)
        {
            for (const auto& event : frameEvents)
            {
                if ((!requireValid || event.IsValid()) && std::strcmp(event.GetName(), eventName) == 0)
                    ++count;
            }
        }
    }
    return count;
#else
    (void)eventName;
    (void)requireValid;
    return 0u;
#endif
}

uint32_t TimelineProfilerSink::GetPendingGpuProfilerEventCountForTesting() const
{
#if NLS_ENABLE_TIMELINE_PROFILER
    return gGPUProfiler.GetCurrentEventCountForTesting();
#else
    return 0u;
#endif
}

uint32_t TimelineProfilerSink::GetPendingGpuProfilerCommandListQueryCountForTesting() const
{
#if NLS_ENABLE_TIMELINE_PROFILER
    return gGPUProfiler.GetPendingCommandListQueryCountForTesting();
#else
    return 0u;
#endif
}

size_t TimelineProfilerSink::GetSubmittedGpuProfilerReadbackCountForTesting() const
{
#if NLS_ENABLE_TIMELINE_PROFILER
    return gGPUProfiler.GetSubmittedReadbackFrameCountForTesting();
#else
    return 0u;
#endif
}

void TimelineProfilerSink::DrawTimeline()
{
#if NLS_ENABLE_TIMELINE_PROFILER
    if (PrepareTimelineUI())
        DrawProfilerHUD();
#endif
}

void TimelineProfilerSink::SetRecordingEnabled(const bool enabled)
{
#if NLS_ENABLE_TIMELINE_PROFILER
    const bool wasRecordingEnabled = m_recordingEnabled;
    m_recordingEnabled = enabled;
    if (enabled && !wasRecordingEnabled)
        Profiler::ReplayGpuContextIfAvailable(*this);
    if (!enabled)
    {
        g_timelineScopeDepth = 0u;
        g_timelineSuppressedScopeDepth = 0u;
        g_timelineFlushedScopeDepth = 0u;
    }
#else
    (void)enabled;
#endif
}

bool TimelineProfilerSink::BeginTraceExport(const std::filesystem::path& path)
{
#if NLS_ENABLE_TIMELINE_PROFILER
    EnsureTimelineProfilerInitialized();
    if (m_traceExport.stream.is_open())
        return m_traceExport.path == path;

    std::error_code error;
    const auto parentPath = path.parent_path();
    if (!parentPath.empty())
        std::filesystem::create_directories(parentPath, error);
    if (error)
        return false;

    m_traceExport.stream.open(path, std::ios::trunc);
    if (!m_traceExport.stream.is_open())
        return false;

    m_traceExport.path = path;
    m_traceExport.baseTime = QueryTimelineTraceCounter();
    m_traceExport.exportedFrameCount = 0u;
    const URange cpuRange = gProfiler.GetFrameRange();
    m_traceExport.lastExportedFrame = cpuRange.End > 0u ? cpuRange.End - 1u : 0u;

    m_traceExport.stream << "{\n\"traceEvents\": [\n";
    m_traceExport.stream << NLS::UI::TimelineProfilerDetail::BuildTraceMetadataEventJson(
        "process_name",
        0u,
        std::nullopt,
        "Track") << ",\n";
    for (const ::Profiler::EventTrack& track : gProfiler.GetTracks())
    {
        m_traceExport.stream << NLS::UI::TimelineProfilerDetail::BuildTraceMetadataEventJson(
            "thread_name",
            0u,
            track.Index,
            track.Name) << ",\n";
    }
    return true;
#else
    (void)path;
    return false;
#endif
}

uint32_t TimelineProfilerSink::UpdateTraceExport(const uint32_t maxFrameCount)
{
#if NLS_ENABLE_TIMELINE_PROFILER
    if (!m_traceExport.stream.is_open() || maxFrameCount == 0u)
        return 0u;

    const double ticksToMs = QueryTimelineTraceTicksToMs();
    const URange cpuRange = gProfiler.GetFrameRange();
    const auto exportRange = NLS::UI::TimelineProfilerDetail::ResolveBudgetedTraceFrameExportRange(
        { cpuRange.Begin, cpuRange.End },
        m_traceExport.lastExportedFrame,
        maxFrameCount);

    uint32_t exportedThisCall = 0u;
    for (uint32_t frameIndex = exportRange.Begin; frameIndex < exportRange.End; ++frameIndex)
    {
        for (const ::Profiler::EventTrack& track : gProfiler.GetTracks())
        {
            for (const ::ProfilerEvent& event : track.GetFrameData(frameIndex))
            {
                const auto eventJson = NLS::UI::TimelineProfilerDetail::BuildTraceDurationEventJson(
                    track.Index,
                    event.TicksBegin,
                    event.TicksEnd,
                    m_traceExport.baseTime,
                    ticksToMs,
                    event.GetName(),
                    true);
                if (!eventJson.has_value())
                    continue;

                m_traceExport.stream << *eventJson << ",\n";
            }
        }
        m_traceExport.lastExportedFrame = frameIndex;
        ++m_traceExport.exportedFrameCount;
        ++exportedThisCall;
    }
    return exportedThisCall;
#else
    (void)maxFrameCount;
    return 0u;
#endif
}

void TimelineProfilerSink::EndTraceExport()
{
#if NLS_ENABLE_TIMELINE_PROFILER
    if (!m_traceExport.stream.is_open())
        return;

    m_traceExport.stream << "{}]\n}";
    m_traceExport.stream.close();
    m_traceExport.path.clear();
#endif
}

bool TimelineProfilerSink::IsTraceExportOpen() const
{
#if NLS_ENABLE_TIMELINE_PROFILER
    return m_traceExport.stream.is_open();
#else
    return false;
#endif
}

uint32_t TimelineProfilerSink::GetTraceExportedFrameCount() const
{
#if NLS_ENABLE_TIMELINE_PROFILER
    return m_traceExport.exportedFrameCount;
#else
    return 0u;
#endif
}

size_t TimelineProfilerSink::GetRecordedTrackCountForTesting() const
{
#if NLS_ENABLE_TIMELINE_PROFILER
    return gProfiler.GetTracks().size();
#else
    return 0u;
#endif
}

ProfilerDestinationState TimelineProfilerSink::GetState() const
{
#if NLS_ENABLE_TIMELINE_PROFILER
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
            (m_gpuScopesAvailable ? ProfilerCapability_GPUScopes : ProfilerCapability_None),
        (m_gpuInitialized && !m_gpuScopesAvailable)
            ? "TimelineProfiler GPU scopes are unavailable for the current D3D12 queue set. A direct queue must be unique, copy queues require timestamp support, and compute queues are intentionally not profiled to avoid shared timestamp-heap synchronization ambiguity."
            : ""
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
