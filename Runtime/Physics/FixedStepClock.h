#pragma once

#include <cstdint>
#include <string>
#include "PhysicsDef.h"

namespace NLS::Physics
{
struct FixedStepSettings
{
    double fixedDeltaTime = 1.0 / 60.0;
    uint32_t maxSubsteps = 8;
    double maximumAccumulatedTime = 0.25;
};

struct FixedStepBatch
{
    uint32_t stepCount = 0;
    double fixedDeltaTime = 1.0 / 60.0;
    double interpolationAlpha = 0;
    double droppedTime = 0;
};

// Owns simulation debt only. Time scaling belongs to the caller and manual
// single steps deliberately do not consume this clock's fractional remainder.
class NLS_PHYSICS_API FixedStepClock final
{
public:
    bool Configure(const FixedStepSettings& settings, std::string& error);
    FixedStepBatch Advance(double scaledDeltaTime);
    void Reset();
    const FixedStepSettings& GetSettings() const { return m_settings; }
    double GetAccumulatedTime() const { return m_accumulatedTime; }
    double GetDroppedTime() const { return m_droppedTime; }

private:
    FixedStepSettings m_settings;
    double m_accumulatedTime = 0;
    double m_droppedTime = 0;
};
}
