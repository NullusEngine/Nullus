#pragma once

#include "Profiling/Profiler.h"

#ifndef NLS_ENABLE_TRACY
#define NLS_ENABLE_TRACY 0
#endif

namespace NLS::Base::Profiling
{
class NLS_BASE_API TracyProfiler final : public IProfilerDestination
{
public:
    void BeginScope(const ProfilerScopeEvent& event) override;
    void EndScope(const ProfilerScopeEvent& event) override;
    ProfilerDestinationState GetState() const override;

    static bool IsConnected();
};
}
