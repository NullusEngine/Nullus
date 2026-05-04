#include "Core/SceneCameraFocus.h"

#include <algorithm>
#include <cmath>

namespace NLS::Editor::Core
{
namespace
{
constexpr float kMinimumFocusDistance = 0.05f;
constexpr float kZoomDistanceFractionPerWheelStep = 0.12f;

float ClampFocusDistance(const float distance)
{
    if (!std::isfinite(distance))
        return kMinimumFocusDistance;

    return std::max(distance, kMinimumFocusDistance);
}
}

float GetMinimumSceneCameraFocusDistance()
{
    return kMinimumFocusDistance;
}

SceneCameraFocusState EnsureSceneCameraFocus(
    const SceneCameraFocusState& p_focus,
    const Maths::Vector3& p_cameraPosition,
    const Maths::Vector3& p_cameraForward,
    const float p_defaultDistance)
{
    if (p_focus.hasFocus)
    {
        return {
            p_focus.focusPoint,
            ClampFocusDistance(p_focus.focusDistance),
            true
        };
    }

    const float distance = ClampFocusDistance(p_defaultDistance);
    const Maths::Vector3 forward = Maths::Vector3::Normalize(p_cameraForward);
    return {
        p_cameraPosition + forward * distance,
        distance,
        true
    };
}

SceneCameraFocusState UpdateSceneCameraFocusAfterRotation(
    const SceneCameraFocusState& p_focus,
    const Maths::Vector3& p_cameraPosition,
    const Maths::Vector3& p_cameraForward)
{
    const float distance = ClampFocusDistance(p_focus.focusDistance);
    const Maths::Vector3 forward = Maths::Vector3::Normalize(p_cameraForward);
    return {
        p_cameraPosition + forward * distance,
        distance,
        true
    };
}

SceneCameraFocusState UpdateSceneCameraFocusAfterPan(
    const SceneCameraFocusState& p_focus,
    const Maths::Vector3& p_worldDelta)
{
    return {
        p_focus.focusPoint + p_worldDelta,
        ClampFocusDistance(p_focus.focusDistance),
        true
    };
}

SceneCameraFocusState UpdateSceneCameraFocusAfterZoom(
    const SceneCameraFocusState& p_focus,
    const Maths::Vector3& p_cameraPosition)
{
    return {
        p_focus.focusPoint,
        ClampFocusDistance(Maths::Vector3::Distance(p_cameraPosition, p_focus.focusPoint)),
        true
    };
}

Maths::Vector3 GetSceneCameraPanDelta(
    const Maths::Quaternion& p_cameraRotation,
    const Maths::Vector2& p_mouseDelta,
    const float p_focusDistance,
    const float p_verticalFovDegrees,
    const float p_viewportHeight)
{
    if (p_viewportHeight <= 0.0f)
        return Maths::Vector3::Zero;

    const float distance = ClampFocusDistance(p_focusDistance);
    const float fovRadians = p_verticalFovDegrees * 3.14159265358979323846f / 180.0f;
    const float worldPerPixel = 2.0f * distance * std::tan(fovRadians * 0.5f) / p_viewportHeight;
    const Maths::Vector3 right = p_cameraRotation * Maths::Vector3::Right;
    const Maths::Vector3 up = p_cameraRotation * Maths::Vector3::Up;

    return right * (p_mouseDelta.x * worldPerPixel) - up * (p_mouseDelta.y * worldPerPixel);
}

Maths::Vector3 GetSceneCameraZoomDelta(
    const Maths::Vector3& p_cameraForward,
    const float p_mouseWheelDelta,
    const float p_focusDistance)
{
    if (p_mouseWheelDelta == 0.0f)
        return Maths::Vector3::Zero;

    const float distance = ClampFocusDistance(p_focusDistance);
    const float maxForwardStep = std::max(0.0f, distance - kMinimumFocusDistance);
    float step = distance * kZoomDistanceFractionPerWheelStep * p_mouseWheelDelta;
    if (step > 0.0f)
        step = std::min(step, maxForwardStep);

    return Maths::Vector3::Normalize(p_cameraForward) * step;
}
}
