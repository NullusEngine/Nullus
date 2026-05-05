#include "Profiling/ProfilerScope.h"

#include <utility>

namespace NLS::Base::Profiling
{
ProfilerScope::ProfilerScope(const std::string_view name, const std::string_view sourceFunction)
    : m_event(Profiler::BeginScope(name, sourceFunction))
{
}

ProfilerScope::~ProfilerScope()
{
    Profiler::EndScope(m_event);
}

ProfilerScope::ProfilerScope(ProfilerScope&& other) noexcept
    : m_event(std::move(other.m_event))
{
    other.m_event.active = false;
}

ProfilerScope& ProfilerScope::operator=(ProfilerScope&& other) noexcept
{
    if (this == &other)
        return *this;

    Profiler::EndScope(m_event);
    m_event = std::move(other.m_event);
    other.m_event.active = false;
    return *this;
}

ProfilerGpuScope::ProfilerGpuScope(
    void* nativeCommandBuffer,
    const std::string_view name,
    const std::string_view sourceFunction)
    : m_event(Profiler::BeginGpuScope(nativeCommandBuffer, name, sourceFunction))
{
}

ProfilerGpuScope::~ProfilerGpuScope()
{
    Profiler::EndGpuScope(m_event);
}

ProfilerGpuScope::ProfilerGpuScope(ProfilerGpuScope&& other) noexcept
    : m_event(std::move(other.m_event))
{
    other.m_event.active = false;
}

ProfilerGpuScope& ProfilerGpuScope::operator=(ProfilerGpuScope&& other) noexcept
{
    if (this == &other)
        return *this;

    Profiler::EndGpuScope(m_event);
    m_event = std::move(other.m_event);
    other.m_event.active = false;
    return *this;
}
}
