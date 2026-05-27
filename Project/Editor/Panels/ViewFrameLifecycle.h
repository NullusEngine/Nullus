#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "Math/Quaternion.h"
#include "Vector3.h"
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

    inline bool ShouldDeferRetirementAwareViewResizeUntilTelemetryAvailable(
        const std::pair<uint16_t, uint16_t>& requestedSize,
        const std::pair<uint16_t, uint16_t>& activeSize,
        const bool requiresRetiredFrameConsumption,
        const bool threadedTelemetryAvailable)
    {
        return requiresRetiredFrameConsumption &&
            !threadedTelemetryAvailable &&
            requestedSize != activeSize;
    }

    inline bool ShouldDrainAfterRetirementAwareViewRender(
        const bool requiresRetiredFrameConsumption,
        const bool requiresImmediateReadback,
        const bool resizedViewThisFrame = false,
        const bool requiresSynchronizedPresentation = false)
    {
        return requiresRetiredFrameConsumption &&
            (requiresImmediateReadback || resizedViewThisFrame || requiresSynchronizedPresentation);
    }

    inline bool DidThreadedFramePublishAdvance(
        const std::optional<Render::Context::ThreadedFrameTelemetry>& beforeFrameTelemetry,
        const std::optional<uint64_t>& previousAvailablePublishedFrameCount,
        const Render::Context::ThreadedFrameTelemetry& afterFrameTelemetry)
    {
        const std::optional<uint64_t> baselinePublishedFrameCount = beforeFrameTelemetry.has_value()
            ? std::optional<uint64_t>(beforeFrameTelemetry->publishedFrameCount)
            : previousAvailablePublishedFrameCount;
        return baselinePublishedFrameCount.has_value() &&
            afterFrameTelemetry.publishedFrameCount > baselinePublishedFrameCount.value();
    }

    inline bool HasSceneViewCameraMotionForPresentation(
        const Maths::Vector3& previousPosition,
        const Maths::Quaternion& previousRotation,
        const Maths::Vector3& currentPosition,
        const Maths::Quaternion& currentRotation)
    {
        constexpr float kPositionEpsilon = 1e-5f;
        constexpr float kRotationEpsilon = 1e-5f;
        const bool rotationChanged =
            std::fabs(previousRotation.x - currentRotation.x) > kRotationEpsilon ||
            std::fabs(previousRotation.y - currentRotation.y) > kRotationEpsilon ||
            std::fabs(previousRotation.z - currentRotation.z) > kRotationEpsilon ||
            std::fabs(previousRotation.w - currentRotation.w) > kRotationEpsilon;

        return Maths::Vector3::Distance(previousPosition, currentPosition) > kPositionEpsilon ||
            rotationChanged;
    }

    inline bool ShouldDelayRetirementAwareViewOverlayMatrices(
        const bool requiresRetiredFrameConsumption,
        const bool requiresImmediateReadback,
        const bool threadedRendering)
    {
        (void)requiresRetiredFrameConsumption;
        (void)requiresImmediateReadback;
        (void)threadedRendering;
        return false;
    }

    inline bool ShouldSceneViewRequestImmediatePickingReadback()
    {
        return false;
    }

    inline bool ShouldSceneViewSynchronizeRetiredFramePresentation()
    {
        return false;
    }

    inline bool ShouldKeepStartupValidationFocusActive(
        const std::string_view validationFocusView,
        const uint64_t elapsedFrames,
        const uint64_t warmupFrameCount)
    {
        return !validationFocusView.empty() && elapsedFrames < warmupFrameCount;
    }
}
