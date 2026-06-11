#pragma once

#include <cstdint>

#include "UIDef.h"

namespace NLS::UI::Profiling
{
inline constexpr uint32_t kTimelineProfilerHistoryFrameCount = 32u;
inline constexpr uint32_t kTimelineProfilerInternalCpuStackDepth = 32u;
inline constexpr uint32_t kTimelineProfilerReservedFrameRootDepth = 1u;
inline constexpr uint32_t kTimelineProfilerMaxCpuScopeDepth =
    kTimelineProfilerInternalCpuStackDepth - kTimelineProfilerReservedFrameRootDepth;
static_assert(kTimelineProfilerMaxCpuScopeDepth < kTimelineProfilerInternalCpuStackDepth);
}
