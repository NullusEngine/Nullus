#include "Profiling/TracyProfiler.h"

#if NLS_ENABLE_TRACY && !defined(NLS_TRACY_UNAVAILABLE)
#include <cstdint>
#include <string_view>
#include <vector>

#include <tracy/TracyC.h>

namespace
{
thread_local std::vector<TracyCZoneCtx> g_tracyScopeStack;
}
#endif

namespace NLS::Base::Profiling
{
void TracyProfiler::BeginScope(const ProfilerScopeEvent& event)
{
#if NLS_ENABLE_TRACY && !defined(NLS_TRACY_UNAVAILABLE)
    if (!IsConnected())
        return;

    const std::string_view name = event.name;
    const std::string_view function = event.sourceFunction;
    const uint64_t sourceLocation = ___tracy_alloc_srcloc_name(
        0u,
        "",
        0u,
        function.data(),
        function.size(),
        name.data(),
        name.size(),
        0u);
    g_tracyScopeStack.push_back(___tracy_emit_zone_begin_alloc(sourceLocation, 1));
#else
    (void)event;
#endif
}

void TracyProfiler::EndScope(const ProfilerScopeEvent& event)
{
#if NLS_ENABLE_TRACY && !defined(NLS_TRACY_UNAVAILABLE)
    (void)event;
    if (g_tracyScopeStack.empty())
        return;

    const auto scope = g_tracyScopeStack.back();
    g_tracyScopeStack.pop_back();
    ___tracy_emit_zone_end(scope);
#else
    (void)event;
#endif
}

ProfilerDestinationState TracyProfiler::GetState() const
{
#if NLS_ENABLE_TRACY && !defined(NLS_TRACY_UNAVAILABLE)
    const bool connected = IsConnected();
    return {
        ProfilerDestinationId::Tracy,
        connected,
        connected ? ProfilerAvailability::Available : ProfilerAvailability::Disabled,
        connected ? ProfilerCapability_CPUScopes : ProfilerCapability_None,
        connected ? "" : "Tracy profiler is not connected"
    };
#else
    return {
        ProfilerDestinationId::Tracy,
        false,
        ProfilerAvailability::Unavailable,
        ProfilerCapability_None,
        "Tracy destination is not enabled or ThirdParty/Tracy is unavailable"
    };
#endif
}

bool TracyProfiler::IsConnected()
{
#if NLS_ENABLE_TRACY && !defined(NLS_TRACY_UNAVAILABLE)
    return TracyCIsConnected != 0;
#else
    return false;
#endif
}
}
