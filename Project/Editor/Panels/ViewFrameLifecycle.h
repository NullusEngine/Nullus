#pragma once

#include <cstdint>
#include <utility>

#include "Rendering/Context/ThreadedRenderingLifecycle.h"

namespace NLS::Editor::Panels
{
    enum class ViewportOverlayLifecyclePhase
    {
        BeforeViewRender,
        AfterWidgetDraw
    };

    inline bool ShouldApplySceneMutationFromViewportOverlay(
        const ViewportOverlayLifecyclePhase phase)
    {
        return phase == ViewportOverlayLifecyclePhase::BeforeViewRender;
    }

    inline bool ShouldResolveViewportPicking(
        const ViewportOverlayLifecyclePhase phase)
    {
        return phase == ViewportOverlayLifecyclePhase::AfterWidgetDraw;
    }

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

    inline bool ShouldDrainAfterRetirementAwareViewRender(
        const bool requiresRetiredFrameConsumption,
        const bool requiresImmediateReadback)
    {
        return requiresRetiredFrameConsumption && requiresImmediateReadback;
    }

    inline bool ShouldDelayRetirementAwareViewOverlayMatrices(
        const bool requiresRetiredFrameConsumption,
        const bool requiresImmediateReadback,
        const bool threadedRendering)
    {
        return threadedRendering &&
            requiresRetiredFrameConsumption &&
            !requiresImmediateReadback;
    }

    inline bool ShouldSceneViewRequestImmediatePickingReadback()
    {
        return false;
    }
}
