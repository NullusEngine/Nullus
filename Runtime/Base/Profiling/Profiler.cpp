#include "Profiling/Profiler.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <vector>

namespace NLS::Base::Profiling
{
namespace
{
std::mutex g_profilerMutex;
bool g_enabled = false;
std::vector<IProfilerDestination*> g_destinations;
ProfilerSessionStats g_stats;
ProfilerGpuContextEvent g_lastGpuContext;
bool g_hasGpuContext = false;
thread_local uint32_t g_scopeDepth = 0u;
thread_local uint32_t g_gpuScopeDepth = 0u;
thread_local std::string g_threadName;

bool AcceptsCPUScopes(const ProfilerDestinationState& state)
{
    return state.enabled &&
        state.availability == ProfilerAvailability::Available &&
        (state.capabilities & ProfilerCapability_CPUScopes) != 0u;
}

bool AcceptsGPUScopes(const ProfilerDestinationState& state)
{
    return state.enabled &&
        state.availability == ProfilerAvailability::Available &&
        (state.capabilities & ProfilerCapability_GPUScopes) != 0u;
}

bool AcceptsGPUContextInitialization(const ProfilerDestinationState& state)
{
    return state.enabled &&
        state.availability == ProfilerAvailability::Available &&
        ((state.capabilities & ProfilerCapability_GPUScopes) != 0u ||
            (state.capabilities & ProfilerCapability_EditorTimeline) != 0u);
}

std::vector<IProfilerDestination*> GetAvailableDestinations(
    bool (*accepts)(const ProfilerDestinationState&),
    uint64_t* skippedDestinationCount = nullptr)
{
    std::lock_guard lock(g_profilerMutex);
    std::vector<IProfilerDestination*> available;
    available.reserve(g_destinations.size());
    uint64_t skipped = 0u;

    for (auto* destination : g_destinations)
    {
        if (destination == nullptr)
        {
            ++skipped;
            continue;
        }

        if (accepts(destination->GetState()))
        {
            available.push_back(destination);
        }
        else
        {
            ++skipped;
        }
    }

    if (skippedDestinationCount != nullptr)
        *skippedDestinationCount = skipped;

    return available;
}
}

void IProfilerDestination::BeginGpuScope(const ProfilerGpuScopeEvent& event)
{
    (void)event;
}

void IProfilerDestination::EndGpuScope(const ProfilerGpuScopeEvent& event)
{
    (void)event;
}

void IProfilerDestination::InitializeGpuContext(const ProfilerGpuContextEvent& event)
{
    (void)event;
}

void IProfilerDestination::SubmitGpuCommandLists(const ProfilerGpuCommandListSubmitEvent& event)
{
    (void)event;
}

void Profiler::SetEnabled(const bool enabled)
{
    std::lock_guard lock(g_profilerMutex);
    g_enabled = enabled;
}

bool Profiler::IsEnabled()
{
    std::lock_guard lock(g_profilerMutex);
    return g_enabled;
}

void Profiler::RegisterDestination(IProfilerDestination& destination)
{
    std::optional<ProfilerGpuContextEvent> contextToReplay;
    {
        std::lock_guard lock(g_profilerMutex);
        const auto found = std::find(g_destinations.begin(), g_destinations.end(), &destination);
        if (found == g_destinations.end())
            g_destinations.push_back(&destination);

        if (g_hasGpuContext && AcceptsGPUContextInitialization(destination.GetState()))
            contextToReplay = g_lastGpuContext;
    }

    if (contextToReplay.has_value())
        destination.InitializeGpuContext(contextToReplay.value());
}

void Profiler::UnregisterDestination(IProfilerDestination& destination)
{
    std::lock_guard lock(g_profilerMutex);
    g_destinations.erase(
        std::remove(g_destinations.begin(), g_destinations.end(), &destination),
        g_destinations.end());
}

void Profiler::ResetForTesting()
{
    std::lock_guard lock(g_profilerMutex);
    g_enabled = false;
    g_destinations.clear();
    g_stats = {};
    g_lastGpuContext = {};
    g_hasGpuContext = false;
    g_scopeDepth = 0u;
    g_gpuScopeDepth = 0u;
    g_threadName.clear();
}

size_t Profiler::GetDestinationCountForTesting()
{
    std::lock_guard lock(g_profilerMutex);
    return g_destinations.size();
}

void Profiler::RegisterThread(const std::string_view name)
{
    g_threadName = std::string(name);
}

ProfilerScopeEvent Profiler::BeginScope(const std::string_view name, const std::string_view sourceFunction)
{
    ProfilerScopeEvent event;
    event.name = name.empty() ? std::string(sourceFunction) : std::string(name);
    event.sourceFunction = std::string(sourceFunction);
    event.threadName = g_threadName;
    event.depth = g_scopeDepth;

    if (event.name.empty())
        event.name = "Unnamed Scope";

    const bool enabled = IsEnabled();
    uint64_t skippedDestinationCount = 0u;
    auto destinations = enabled
        ? GetAvailableDestinations(AcceptsCPUScopes, &skippedDestinationCount)
        : std::vector<IProfilerDestination*> {};
    if (!enabled || destinations.empty())
    {
        std::lock_guard lock(g_profilerMutex);
        ++g_stats.droppedEventCount;
        g_stats.droppedEventCount += skippedDestinationCount;
        return event;
    }

    event.destinations = destinations;
    event.active = true;
    for (auto* destination : destinations)
        destination->BeginScope(event);

    {
        std::lock_guard lock(g_profilerMutex);
        ++g_stats.acceptedEventCount;
        g_stats.droppedEventCount += skippedDestinationCount;
    }

    ++g_scopeDepth;
    return event;
}

void Profiler::EndScope(const ProfilerScopeEvent& event)
{
    if (!event.active)
        return;

    for (auto* destination : event.destinations)
    {
        if (destination != nullptr)
            destination->EndScope(event);
    }

    if (g_scopeDepth > 0u)
        --g_scopeDepth;
}

ProfilerGpuScopeEvent Profiler::BeginGpuScope(
    void* nativeCommandBuffer,
    const std::string_view name,
    const std::string_view sourceFunction)
{
    ProfilerGpuScopeEvent event;
    event.nativeCommandBuffer = nativeCommandBuffer;
    event.name = name.empty() ? std::string(sourceFunction) : std::string(name);
    event.sourceFunction = std::string(sourceFunction);
    event.threadName = g_threadName;
    event.depth = g_gpuScopeDepth;

    if (event.name.empty())
        event.name = "Unnamed GPU Scope";

    const bool enabled = IsEnabled();
    uint64_t skippedDestinationCount = 0u;
    auto destinations = enabled
        ? GetAvailableDestinations(AcceptsGPUScopes, &skippedDestinationCount)
        : std::vector<IProfilerDestination*> {};
    if (!enabled || destinations.empty())
    {
        std::lock_guard lock(g_profilerMutex);
        ++g_stats.droppedEventCount;
        g_stats.droppedEventCount += skippedDestinationCount;
        return event;
    }

    event.destinations = destinations;
    event.active = true;
    for (auto* destination : destinations)
        destination->BeginGpuScope(event);

    {
        std::lock_guard lock(g_profilerMutex);
        ++g_stats.acceptedEventCount;
        g_stats.droppedEventCount += skippedDestinationCount;
    }

    ++g_gpuScopeDepth;
    return event;
}

void Profiler::EndGpuScope(const ProfilerGpuScopeEvent& event)
{
    if (!event.active)
        return;

    for (auto* destination : event.destinations)
    {
        if (destination != nullptr)
            destination->EndGpuScope(event);
    }

    if (g_gpuScopeDepth > 0u)
        --g_gpuScopeDepth;
}

void Profiler::InitializeGpuContext(const ProfilerGpuContextEvent& event)
{
    {
        std::lock_guard lock(g_profilerMutex);
        g_lastGpuContext = event;
        g_hasGpuContext = true;
    }

    if (!IsEnabled())
        return;

    auto destinations = GetAvailableDestinations(AcceptsGPUContextInitialization);
    for (auto* destination : destinations)
        destination->InitializeGpuContext(event);
}

void Profiler::SubmitGpuCommandLists(const ProfilerGpuCommandListSubmitEvent& event)
{
    if (!IsEnabled())
        return;

    auto destinations = GetAvailableDestinations(AcceptsGPUScopes);
    for (auto* destination : destinations)
        destination->SubmitGpuCommandLists(event);
}

ProfilerSessionStats Profiler::GetSessionStats()
{
    std::lock_guard lock(g_profilerMutex);
    return g_stats;
}
}
