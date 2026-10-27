#include "VfxGraph.h"

#include <cmath>

namespace NLS::VFX
{
bool ValidateSystemTiming(const SystemTimingSettings& settings,std::string& error)
{
    if (!std::isfinite(settings.fixedTickDelta) || settings.fixedTickDelta < 0.0001 || settings.fixedTickDelta > 1 ||
        !std::isfinite(settings.warmupTickDelta) || settings.warmupTickDelta < 0 || settings.warmupTickDelta > 1 ||
        settings.warmupTickCount > kMaxSystemWarmupTicks || (settings.warmupTickCount && settings.warmupTickDelta < 0.0001))
    { error = "System timing requires fixed delta 0.0001-1 s, warmup delta 0-1 s and at most 1024 warmup ticks"; return false; }
    if (settings.fixedTick && settings.WarmupTime()/settings.fixedTickDelta > kMaxSystemWarmupTicks)
    { error = "System warmup exceeds the 1024 resolved simulation tick budget"; return false; }
    error.clear(); return true;
}
bool SetSystemWarmupTime(SystemTimingSettings& settings,double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0 || !std::isfinite(settings.warmupTickDelta) || settings.warmupTickDelta < 0) return false;
    auto candidate = settings;
    const double count = candidate.warmupTickDelta < 0.0001 ? 0 : std::floor(seconds/candidate.warmupTickDelta);
    if (count > kMaxSystemWarmupTicks) return false;
    candidate.warmupTickCount = static_cast<uint32_t>(count);
    if (candidate.warmupTickDelta < 0.0001) candidate.warmupTickDelta = 0;
    std::string error;
    if (!ValidateSystemTiming(candidate,error)) return false;
    settings = candidate; return true;
}
}
