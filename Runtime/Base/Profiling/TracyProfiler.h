#pragma once

#include "Profiling/Profiler.h"

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
