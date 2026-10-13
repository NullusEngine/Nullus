#pragma once

#include "Profiling/Profiler.h"

#include <memory>

#ifndef NLS_ENABLE_TRACY
#define NLS_ENABLE_TRACY 0
#endif

namespace NLS::Base::Profiling
{
class NLS_BASE_API TracyProfiler final : public IProfilerDestination
{
public:
    TracyProfiler();
    ~TracyProfiler() override;

    TracyProfiler(const TracyProfiler&) = delete;
    TracyProfiler& operator=(const TracyProfiler&) = delete;
    TracyProfiler(TracyProfiler&&) = delete;
    TracyProfiler& operator=(TracyProfiler&&) = delete;

    void BeginScope(const ProfilerScopeEvent& event) override;
    void EndScope(const ProfilerScopeEvent& event) override;
    void BeginGpuScope(const ProfilerGpuScopeEvent& event) override;
    void EndGpuScope(const ProfilerGpuScopeEvent& event) override;
    void InitializeGpuContext(const ProfilerGpuContextEvent& event) override;
    void ClearGpuContext(void* nativeDevice) override;
    void SubmitGpuCommandLists(const ProfilerGpuCommandListSubmitEvent& event) override;
    ProfilerDestinationState GetState() const override;

    static bool IsConnected();
    static void MarkFrame();
    static void RegisterThread(std::string_view name);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
