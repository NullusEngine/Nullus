#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace NLS::Base::Profiling
{
enum class ProfilerDestinationId
{
    Tracy,
    Timeline,
    Test
};

enum class ProfilerAvailability
{
    Available,
    Disabled,
    Unavailable,
    Unsupported
};

using ProfilerCapabilityFlags = uint32_t;

constexpr ProfilerCapabilityFlags ProfilerCapability_None = 0u;
constexpr ProfilerCapabilityFlags ProfilerCapability_CPUScopes = 1u << 0u;
constexpr ProfilerCapabilityFlags ProfilerCapability_GPUScopes = 1u << 1u;
constexpr ProfilerCapabilityFlags ProfilerCapability_EditorTimeline = 1u << 2u;

struct ProfilerDestinationState
{
    ProfilerDestinationId id = ProfilerDestinationId::Test;
    bool enabled = false;
    ProfilerAvailability availability = ProfilerAvailability::Disabled;
    ProfilerCapabilityFlags capabilities = ProfilerCapability_None;
    std::string lastError;
};

struct ProfilerSessionStats
{
    uint64_t acceptedEventCount = 0u;
    uint64_t droppedEventCount = 0u;
};

struct ProfilerScopeEvent
{
    std::string name;
    std::string sourceFunction;
    std::string threadName;
    uint32_t depth = 0u;
    bool active = false;
};

struct ProfilerGpuScopeEvent
{
    void* nativeCommandBuffer = nullptr;
    std::string name;
    std::string sourceFunction;
    std::string threadName;
    uint32_t depth = 0u;
    bool active = false;
};

struct ProfilerGpuContextEvent
{
    void* nativeDevice = nullptr;
    std::vector<void*> nativeCommandQueues;
    uint32_t frameLatency = 0u;
};

struct ProfilerGpuCommandListSubmitEvent
{
    void* nativeCommandQueue = nullptr;
    std::vector<void*> nativeCommandLists;
};

class IProfilerDestination
{
public:
    virtual ~IProfilerDestination() = default;
    virtual void BeginScope(const ProfilerScopeEvent& event) = 0;
    virtual void EndScope(const ProfilerScopeEvent& event) = 0;
    virtual void BeginGpuScope(const ProfilerGpuScopeEvent& event);
    virtual void EndGpuScope(const ProfilerGpuScopeEvent& event);
    virtual void InitializeGpuContext(const ProfilerGpuContextEvent& event);
    virtual void SubmitGpuCommandLists(const ProfilerGpuCommandListSubmitEvent& event);
    virtual ProfilerDestinationState GetState() const = 0;
};

class Profiler
{
public:
    static void SetEnabled(bool enabled);
    static bool IsEnabled();

    static void RegisterDestination(IProfilerDestination& destination);
    static void UnregisterDestination(IProfilerDestination& destination);
    static void ResetForTesting();
    static size_t GetDestinationCountForTesting();
    static void RegisterThread(std::string_view name);

    static ProfilerScopeEvent BeginScope(std::string_view name, std::string_view sourceFunction);
    static void EndScope(const ProfilerScopeEvent& event);
    static ProfilerGpuScopeEvent BeginGpuScope(
        void* nativeCommandBuffer,
        std::string_view name,
        std::string_view sourceFunction);
    static void EndGpuScope(const ProfilerGpuScopeEvent& event);
    static void InitializeGpuContext(const ProfilerGpuContextEvent& event);
    static void SubmitGpuCommandLists(const ProfilerGpuCommandListSubmitEvent& event);

    static ProfilerSessionStats GetSessionStats();
};

}

#include "Profiling/ProfilerScope.h"

#define NLS_PROFILE_CONCAT_IMPL(a, b) a##b
#define NLS_PROFILE_CONCAT(a, b) NLS_PROFILE_CONCAT_IMPL(a, b)

#define NLS_PROFILE_SCOPE() \
    ::NLS::Base::Profiling::ProfilerScope NLS_PROFILE_CONCAT(nlsProfileScope, __LINE__)(__FUNCTION__, __FUNCTION__)

#define NLS_PROFILE_NAMED_SCOPE(name) \
    ::NLS::Base::Profiling::ProfilerScope NLS_PROFILE_CONCAT(nlsProfileScope, __LINE__)(name, __FUNCTION__)

#define NLS_PROFILE_REGISTER_THREAD(name) \
    ::NLS::Base::Profiling::Profiler::RegisterThread(name)

#define NLS_GPU_PROFILE_SCOPE(nativeCommandBuffer) \
    ::NLS::Base::Profiling::ProfilerGpuScope NLS_PROFILE_CONCAT(nlsGpuProfileScope, __LINE__)(nativeCommandBuffer, __FUNCTION__, __FUNCTION__)

#define NLS_GPU_PROFILE_NAMED_SCOPE(nativeCommandBuffer, name) \
    ::NLS::Base::Profiling::ProfilerGpuScope NLS_PROFILE_CONCAT(nlsGpuProfileScope, __LINE__)(nativeCommandBuffer, name, __FUNCTION__)
