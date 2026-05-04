#pragma once

#include "Math/Quaternion.h"
#include "Math/Vector2.h"
#include "Math/Vector3.h"

namespace NLS::Editor::Core
{
struct SceneCameraFocusState
{
    Maths::Vector3 focusPoint = Maths::Vector3::Zero;
    float focusDistance = 15.0f;
    bool hasFocus = false;
};

float GetMinimumSceneCameraFocusDistance();
SceneCameraFocusState EnsureSceneCameraFocus(
    const SceneCameraFocusState& p_focus,
    const Maths::Vector3& p_cameraPosition,
    const Maths::Vector3& p_cameraForward,
    float p_defaultDistance);
SceneCameraFocusState UpdateSceneCameraFocusAfterRotation(
    const SceneCameraFocusState& p_focus,
    const Maths::Vector3& p_cameraPosition,
    const Maths::Vector3& p_cameraForward);
SceneCameraFocusState UpdateSceneCameraFocusAfterPan(
    const SceneCameraFocusState& p_focus,
    const Maths::Vector3& p_worldDelta);
SceneCameraFocusState UpdateSceneCameraFocusAfterZoom(
    const SceneCameraFocusState& p_focus,
    const Maths::Vector3& p_cameraPosition);
Maths::Vector3 GetSceneCameraPanDelta(
    const Maths::Quaternion& p_cameraRotation,
    const Maths::Vector2& p_mouseDelta,
    float p_focusDistance,
    float p_verticalFovDegrees,
    float p_viewportHeight);
Maths::Vector3 GetSceneCameraZoomDelta(
    const Maths::Vector3& p_cameraForward,
    float p_mouseWheelDelta,
    float p_focusDistance);
}
