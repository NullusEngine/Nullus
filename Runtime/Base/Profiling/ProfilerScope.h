#pragma once

#include "Profiling/Profiler.h"

#include <string_view>

namespace NLS::Base::Profiling
{
class NLS_BASE_API ProfilerScope
{
public:
    ProfilerScope(std::string_view name, std::string_view sourceFunction);
    ~ProfilerScope();

    ProfilerScope(const ProfilerScope&) = delete;
    ProfilerScope& operator=(const ProfilerScope&) = delete;

    ProfilerScope(ProfilerScope&& other) noexcept;
    ProfilerScope& operator=(ProfilerScope&& other) noexcept;

private:
    ProfilerScopeEvent m_event;
};

class NLS_BASE_API ProfilerGpuScope
{
public:
    ProfilerGpuScope(void* nativeCommandBuffer, std::string_view name, std::string_view sourceFunction);
    ~ProfilerGpuScope();

    ProfilerGpuScope(const ProfilerGpuScope&) = delete;
    ProfilerGpuScope& operator=(const ProfilerGpuScope&) = delete;

    ProfilerGpuScope(ProfilerGpuScope&& other) noexcept;
    ProfilerGpuScope& operator=(ProfilerGpuScope&& other) noexcept;

private:
    ProfilerGpuScopeEvent m_event;
};
}
