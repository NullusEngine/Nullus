#pragma once

#include <cstdint>
#include <utility>

#include "Rendering/Context/ThreadedRenderingLifecycle.h"

namespace NLS::Editor::Panels
{
    inline bool ShouldDeferRetirementAwareViewResize(
        const std::pair<uint16_t, uint16_t>& requestedSize,
        const std::pair<uint16_t, uint16_t>& activeSize,
        const bool requiresRetiredFrameConsumption,
        const Render::Context::ThreadedFrameTelemetry& telemetry)
    {
        return requiresRetiredFrameConsumption &&
            requestedSize != activeSize &&
            telemetry.inFlightFrameCount > 0u;
    }

    inline bool ShouldDrainBeforeRetirementAwareViewResize(
        const std::pair<uint16_t, uint16_t>& requestedSize,
        const std::pair<uint16_t, uint16_t>& activeSize,
        const bool requiresRetiredFrameConsumption,
        const Render::Context::ThreadedFrameTelemetry& telemetry)
    {
        return ShouldDeferRetirementAwareViewResize(
            requestedSize,
            activeSize,
            requiresRetiredFrameConsumption,
            telemetry);
    }
}
