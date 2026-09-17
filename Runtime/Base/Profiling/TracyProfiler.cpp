#include "Profiling/TracyProfiler.h"

#if NLS_ENABLE_TRACY && !defined(NLS_TRACY_UNAVAILABLE)
#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <tracy/TracyC.h>

#if defined(_WIN32)
#include <d3d12.h>
#include <tracy/TracyD3D12.hpp>
#endif

namespace
{
thread_local std::vector<TracyCZoneCtx> g_tracyScopeStack;

#if defined(_WIN32)
bool QueueSupportsGpuProfiling(ID3D12CommandQueue* queue, const bool copyQueueTimestampsSupported)
{
    if (queue == nullptr)
        return false;

    const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
    const bool supportedType =
        desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT ||
        desc.Type == D3D12_COMMAND_LIST_TYPE_COMPUTE ||
        (desc.Type == D3D12_COMMAND_LIST_TYPE_COPY && copyQueueTimestampsSupported);
    if (!supportedType)
        return false;

    uint64_t gpuTimestamp = 0u;
    uint64_t cpuTimestamp = 0u;
    uint64_t timestampFrequency = 0u;
    return SUCCEEDED(queue->GetClockCalibration(&gpuTimestamp, &cpuTimestamp)) &&
        SUCCEEDED(queue->GetTimestampFrequency(&timestampFrequency)) &&
        timestampFrequency != 0u;
}

const char* GetQueueContextName(const D3D12_COMMAND_LIST_TYPE type)
{
    switch (type)
    {
    case D3D12_COMMAND_LIST_TYPE_COMPUTE:
        return "DX12 Compute Queue";
    case D3D12_COMMAND_LIST_TYPE_COPY:
        return "DX12 Copy Queue";
    default:
        return "DX12 Graphics Queue";
    }
}
#endif
}
#endif

namespace NLS::Base::Profiling
{
struct TracyProfiler::Impl
{
#if NLS_ENABLE_TRACY && !defined(NLS_TRACY_UNAVAILABLE) && defined(_WIN32)
    struct QueueContext
    {
        ID3D12CommandQueue* queue = nullptr;
        tracy::D3D12QueueCtx* tracyContext = nullptr;
    };

    mutable std::mutex mutex;
    ID3D12Device* device = nullptr;
    std::vector<void*> configuredQueues;
    std::vector<QueueContext> queueContexts;
    std::unordered_map<void*, std::vector<std::unique_ptr<tracy::D3D12ZoneScope>>> scopeStacks;
#endif
};

TracyProfiler::TracyProfiler()
    : m_impl(std::make_unique<Impl>())
{
}

TracyProfiler::~TracyProfiler()
{
    ClearGpuContext(nullptr);
}

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

void TracyProfiler::BeginGpuScope(const ProfilerGpuScopeEvent& event)
{
#if NLS_ENABLE_TRACY && !defined(NLS_TRACY_UNAVAILABLE) && defined(_WIN32)
    if (!IsConnected() || event.nativeCommandBuffer == nullptr || event.nativeCommandQueue == nullptr)
        return;

    std::lock_guard lock(m_impl->mutex);
    const auto found = std::find_if(
        m_impl->queueContexts.begin(),
        m_impl->queueContexts.end(),
        [&](const Impl::QueueContext& context) { return context.queue == event.nativeCommandQueue; });
    if (found == m_impl->queueContexts.end() || found->tracyContext == nullptr)
        return;

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(event.nativeCommandBuffer);
    auto scope = std::make_unique<tracy::D3D12ZoneScope>(
        found->tracyContext,
        0u,
        "",
        0u,
        event.sourceFunction.data(),
        event.sourceFunction.size(),
        event.name.data(),
        event.name.size(),
        commandList,
        static_cast<int32_t>(event.depth),
        true);
    m_impl->scopeStacks[event.nativeCommandBuffer].push_back(std::move(scope));
#else
    (void)event;
#endif
}

void TracyProfiler::EndGpuScope(const ProfilerGpuScopeEvent& event)
{
#if NLS_ENABLE_TRACY && !defined(NLS_TRACY_UNAVAILABLE) && defined(_WIN32)
    if (event.nativeCommandBuffer == nullptr)
        return;

    std::lock_guard lock(m_impl->mutex);
    const auto found = m_impl->scopeStacks.find(event.nativeCommandBuffer);
    if (found == m_impl->scopeStacks.end() || found->second.empty())
        return;

    found->second.pop_back();
    if (found->second.empty())
        m_impl->scopeStacks.erase(found);
#else
    (void)event;
#endif
}

void TracyProfiler::InitializeGpuContext(const ProfilerGpuContextEvent& event)
{
#if NLS_ENABLE_TRACY && !defined(NLS_TRACY_UNAVAILABLE) && defined(_WIN32)
    if (!IsConnected() || event.nativeDevice == nullptr || event.nativeCommandQueues.empty())
        return;

    auto* device = static_cast<ID3D12Device*>(event.nativeDevice);
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3{};
    const bool copyQueueTimestampsSupported =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options3, sizeof(options3))) &&
        options3.CopyQueueTimestampQueriesSupported;

    std::vector<void*> configuredQueues;
    configuredQueues.reserve(event.nativeCommandQueues.size());
    for (void* nativeQueue : event.nativeCommandQueues)
    {
        if (nativeQueue == nullptr)
            continue;
        if (std::find(configuredQueues.begin(), configuredQueues.end(), nativeQueue) == configuredQueues.end())
            configuredQueues.push_back(nativeQueue);
    }
    if (configuredQueues.empty())
        return;

    std::lock_guard lock(m_impl->mutex);
    if (m_impl->device == device && m_impl->configuredQueues == configuredQueues)
        return;

    for (const Impl::QueueContext& queueContext : m_impl->queueContexts)
    {
        if (queueContext.tracyContext != nullptr)
            TracyD3D12Destroy(queueContext.tracyContext);
    }
    m_impl->queueContexts.clear();
    m_impl->scopeStacks.clear();
    m_impl->configuredQueues.clear();
    m_impl->device = nullptr;

    m_impl->device = device;
    m_impl->configuredQueues = configuredQueues;
    for (void* nativeQueue : configuredQueues)
    {
        auto* queue = static_cast<ID3D12CommandQueue*>(nativeQueue);
        if (!QueueSupportsGpuProfiling(queue, copyQueueTimestampsSupported))
            continue;

        tracy::D3D12QueueCtx* tracyContext = TracyD3D12Context(device, queue);
        if (tracyContext == nullptr)
            continue;

        const char* contextName = GetQueueContextName(queue->GetDesc().Type);
        const std::string_view contextNameView(contextName);
        TracyD3D12ContextName(
            tracyContext,
            contextNameView.data(),
            static_cast<uint16_t>(contextNameView.size()));
        m_impl->queueContexts.push_back({ queue, tracyContext });
    }
#else
    (void)event;
#endif
}

void TracyProfiler::ClearGpuContext(void* nativeDevice)
{
#if NLS_ENABLE_TRACY && !defined(NLS_TRACY_UNAVAILABLE) && defined(_WIN32)
    std::lock_guard lock(m_impl->mutex);
    if (nativeDevice != nullptr && m_impl->device != nativeDevice)
        return;

    m_impl->scopeStacks.clear();
    for (const Impl::QueueContext& queueContext : m_impl->queueContexts)
    {
        if (queueContext.tracyContext != nullptr)
            TracyD3D12Destroy(queueContext.tracyContext);
    }
    m_impl->queueContexts.clear();
    m_impl->configuredQueues.clear();
    m_impl->device = nullptr;
#else
    (void)nativeDevice;
#endif
}

void TracyProfiler::SubmitGpuCommandLists(const ProfilerGpuCommandListSubmitEvent& event)
{
#if NLS_ENABLE_TRACY && !defined(NLS_TRACY_UNAVAILABLE) && defined(_WIN32)
    if (!IsConnected() || event.nativeCommandQueue == nullptr || event.nativeCommandLists.empty())
        return;

    std::lock_guard lock(m_impl->mutex);
    const auto found = std::find_if(
        m_impl->queueContexts.begin(),
        m_impl->queueContexts.end(),
        [&](const Impl::QueueContext& context) { return context.queue == event.nativeCommandQueue; });
    if (found == m_impl->queueContexts.end() || found->tracyContext == nullptr)
        return;

    TracyD3D12NewFrame(found->tracyContext);
    TracyD3D12Collect(found->tracyContext);
#else
    (void)event;
#endif
}

ProfilerDestinationState TracyProfiler::GetState() const
{
#if NLS_ENABLE_TRACY && !defined(NLS_TRACY_UNAVAILABLE)
    const bool connected = IsConnected();
    bool gpuAvailable = false;
#if defined(_WIN32)
    if (connected)
    {
        std::lock_guard lock(m_impl->mutex);
        gpuAvailable = !m_impl->queueContexts.empty();
    }
#endif

    ProfilerCapabilityFlags capabilities = ProfilerCapability_None;
    if (connected)
    {
        capabilities |= ProfilerCapability_CPUScopes;
#if defined(_WIN32)
        capabilities |= ProfilerCapability_AcceptsGpuContext;
#endif
        if (gpuAvailable)
            capabilities |= ProfilerCapability_GPUScopes;
    }

    std::string lastError;
    if (!connected)
        lastError = "Tracy profiler is not connected";
    else if (!gpuAvailable)
        lastError = "Tracy GPU profiling is unavailable for the current D3D12 queue set";

    return {
        ProfilerDestinationId::Tracy,
        connected,
        connected ? ProfilerAvailability::Available : ProfilerAvailability::Disabled,
        capabilities,
        std::move(lastError)
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

void TracyProfiler::MarkFrame()
{
#if NLS_ENABLE_TRACY && !defined(NLS_TRACY_UNAVAILABLE)
    if (IsConnected())
        ___tracy_emit_frame_mark(nullptr);
#endif
}
}
