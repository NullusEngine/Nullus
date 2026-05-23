#pragma once

#include <array>

#include "Core/GizmoOperation.h"
#include "Math/Matrix4.h"
#include "Math/Quaternion.h"
#include "Math/Vector2.h"
#include "Math/Vector3.h"
#include "Platform/Windowing/Inputs/EKeyState.h"

namespace NLS::Engine { class GameObject; }
namespace NLS::Render::Entities { class Camera; }

namespace NLS::Editor::Core
{
enum class SceneViewGizmoOperation
{
    Translate,
    Rotate,
    Scale
};

enum class SceneViewGizmoPivot
{
    Pivot,
    Center
};

enum class SceneViewGizmoSpace
{
    Global,
    Local
};

using SceneViewGizmoMatrix = std::array<float, 16>;

struct SceneViewGizmoInteraction
{
    bool isHovered = false;
    bool isUsing = false;
    bool isViewHovered = false;
    bool isViewUsing = false;
};

struct SceneViewViewGizmoRect
{
    Maths::Vector2 position {};
    Maths::Vector2 size {};
};

struct SceneViewCameraTransform
{
    Maths::Vector3 position {};
    Maths::Quaternion rotation {};
};

struct SceneViewGizmoMatrices
{
    SceneViewGizmoMatrix view {};
    SceneViewGizmoMatrix projection {};
    SceneViewGizmoMatrix model {};
};

SceneViewGizmoOperation ToImGuizmoOperation(EGizmoOperation p_operation);
SceneViewGizmoPivot ToggleGizmoPivot(SceneViewGizmoPivot p_pivot);
SceneViewGizmoSpace ToggleGizmoSpace(SceneViewGizmoSpace p_space);
float GetSnapValue(EGizmoOperation p_operation);
SceneViewGizmoMatrix ToImGuizmoMatrix(const Maths::Matrix4& p_matrix);
Maths::Matrix4 FromImGuizmoMatrix(const SceneViewGizmoMatrix& p_matrix);
SceneViewCameraTransform GetCameraTransformFromViewMatrix(const SceneViewGizmoMatrix& p_viewMatrix);
Maths::Vector3 GetCameraForwardFromImGuizmoViewTargetDirection(const Maths::Vector3& p_targetDirection);
bool ShouldApplyViewGizmoCameraTransform(
    bool p_isViewUsing,
    bool p_isLeftMouseDown,
    const Maths::Vector2& p_mouseDelta);
bool ShouldCancelViewGizmoCameraTransform(bool p_isCameraControlActive, bool p_isViewUsing);
bool IsViewGizmoOppositeDirectionJump(
    const Maths::Quaternion& p_currentRotation,
    const Maths::Quaternion& p_candidateRotation);
SceneViewCameraTransform StabilizeViewGizmoCameraTransform(
    const SceneViewCameraTransform& p_currentTransform,
    const SceneViewCameraTransform& p_candidateTransform,
    float p_cameraLength,
    const Maths::Vector3& p_orbitFocus,
    const Maths::Vector3* p_targetForward = nullptr);
Maths::Vector3 GetGameObjectGizmoPivotPosition(const Engine::GameObject& p_actor, SceneViewGizmoPivot p_pivot);
SceneViewViewGizmoRect GetSceneViewViewGizmoRect(
    const Maths::Vector2& p_viewMin,
    const Maths::Vector2& p_viewMax);
SceneViewGizmoMatrix GetGameObjectWorldGizmoMatrix(
    const Engine::GameObject& p_actor,
    SceneViewGizmoPivot p_pivot = SceneViewGizmoPivot::Pivot);
void ApplyGameObjectWorldGizmoMatrix(
    Engine::GameObject& p_actor,
    const SceneViewGizmoMatrix& p_matrix,
    EGizmoOperation p_operation = EGizmoOperation::TRANSLATE,
    SceneViewGizmoPivot p_pivot = SceneViewGizmoPivot::Pivot);
bool IsSnapModifierActive(
    Windowing::Inputs::EKeyState p_leftControl,
    Windowing::Inputs::EKeyState p_rightControl);
bool ShouldSuppressScenePicking(const SceneViewGizmoInteraction& p_interaction);
}
