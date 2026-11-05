#include "FixedStepClock.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace NLS::Physics
{
bool FixedStepClock::Configure(const FixedStepSettings& settings, std::string& error)
{
    if (!std::isfinite(settings.fixedDeltaTime) || settings.fixedDeltaTime <= 0 ||
        !std::isfinite(settings.maximumAccumulatedTime) ||
        settings.maximumAccumulatedTime < settings.fixedDeltaTime || settings.maxSubsteps == 0 || settings.maxSubsteps > 1024)
    {
        error = "Physics timing requires a positive finite step, a step budget of 1..1024, and an accumulation limit at least as large as one step.";
        return false;
    }
    m_settings = settings;
    // Preserve pending time when the project changes its fixed interval. The
    // next Advance applies the new budget and accounts for any discarded debt.
    error.clear();
    return true;
}

FixedStepBatch FixedStepClock::Advance(double scaledDeltaTime)
{
    FixedStepBatch batch;
    batch.fixedDeltaTime = m_settings.fixedDeltaTime;
    const double delta = std::isfinite(scaledDeltaTime) ? (std::max)(0.0, scaledDeltaTime) : 0;
    const double pending = (std::min)(m_accumulatedTime, m_settings.maximumAccumulatedTime);
    batch.droppedTime = m_accumulatedTime - pending;
    const double accepted = (std::min)(delta, m_settings.maximumAccumulatedTime - pending);
    m_accumulatedTime = pending + accepted;
    batch.droppedTime += delta - accepted;
    const double tolerance = (std::min)(batch.fixedDeltaTime * 1e-6, 1e-8);
    while (batch.stepCount < m_settings.maxSubsteps &&
        (m_accumulatedTime >= batch.fixedDeltaTime || batch.fixedDeltaTime - m_accumulatedTime <= tolerance))
    {
        m_accumulatedTime = (std::max)(0.0, m_accumulatedTime - batch.fixedDeltaTime);
        ++batch.stepCount;
    }
    if (m_accumulatedTime >= batch.fixedDeltaTime)
    {
        const double remainder = std::fmod(m_accumulatedTime, batch.fixedDeltaTime);
        batch.droppedTime += m_accumulatedTime - remainder;
        m_accumulatedTime = remainder;
    }
    const double headroom = (std::numeric_limits<double>::max)() - m_droppedTime;
    m_droppedTime += (std::min)(batch.droppedTime, headroom);
    batch.interpolationAlpha = std::clamp(m_accumulatedTime / batch.fixedDeltaTime, 0.0, 1.0);
    return batch;
}

void FixedStepClock::Reset()
{
    m_accumulatedTime = 0;
    m_droppedTime = 0;
}
}
