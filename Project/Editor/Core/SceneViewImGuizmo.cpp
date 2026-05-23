#include "Core/SceneViewImGuizmo.h"

#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Components/TransformComponent.h"
#include "GameObject.h"
#include "Math/Matrix3.h"
#include "Math/Vector3.h"
#include "Math/Vector4.h"
#include "Settings/EditorSettings.h"

namespace NLS::Editor::Core
{
namespace
{
constexpr float kViewGizmoSize = 96.0f;
constexpr float kViewGizmoMargin = 8.0f;
constexpr float kOppositeViewYawStepRadians = 0.12f;
constexpr float kMinViewGizmoOrbitLength = 0.0001f;
constexpr float kViewGizmoMaxPitchSin = 0.985f;
constexpr float kMinViewGizmoHorizontalLength = 0.0001f;
constexpr float kViewGizmoOppositeHorizontalDot = -0.25f;
constexpr float kViewGizmoTargetStepBlend = 0.2f;

struct WorldBounds
{
    Maths::Vector3 min;
    Maths::Vector3 max;
};

float ClampUnit(const float value)
{
    return std::clamp(value, -1.0f, 1.0f);
}

Maths::Vector3 ProjectHorizontal(const Maths::Vector3& direction)
{
    return {direction.x, 0.0f, direction.z};
}

Maths::Vector3 GetHorizontalDirection(
    const Maths::Vector3& direction,
    const Maths::Vector3& fallback)
{
    const Maths::Vector3 horizontal = ProjectHorizontal(direction);
    if (Maths::Vector3::Length(horizontal) > kMinViewGizmoHorizontalLength)
        return Maths::Vector3::Normalize(horizontal);

    const Maths::Vector3 fallbackHorizontal = ProjectHorizontal(fallback);
    if (Maths::Vector3::Length(fallbackHorizontal) > kMinViewGizmoHorizontalLength)
        return Maths::Vector3::Normalize(fallbackHorizontal);

    return Maths::Vector3::Forward;
}

Maths::Vector3 ApplyPitchLimit(
    const Maths::Vector3& forward,
    const Maths::Vector3& fallbackHorizontal)
{
    const float y = std::clamp(forward.y, -kViewGizmoMaxPitchSin, kViewGizmoMaxPitchSin);
    const Maths::Vector3 horizontalDirection = GetHorizontalDirection(forward, fallbackHorizontal);
    const float horizontalLength = std::sqrt(std::max(0.0f, 1.0f - y * y));

    return Maths::Vector3::Normalize({
        horizontalDirection.x * horizontalLength,
        y,
        horizontalDirection.z * horizontalLength
    });
}

bool ShouldRouteViewGizmoAroundWorldY(
    const Maths::Vector3& currentForward,
    const Maths::Vector3& candidateForward)
{
    const Maths::Vector3 currentHorizontal = ProjectHorizontal(currentForward);
    const Maths::Vector3 candidateHorizontal = ProjectHorizontal(candidateForward);
    if (Maths::Vector3::Length(currentHorizontal) <= kMinViewGizmoHorizontalLength ||
        Maths::Vector3::Length(candidateHorizontal) <= kMinViewGizmoHorizontalLength)
    {
        return false;
    }

    const float horizontalDot = Maths::Vector3::Dot(
        Maths::Vector3::Normalize(currentHorizontal),
        Maths::Vector3::Normalize(candidateHorizontal));
    return horizontalDot < kViewGizmoOppositeHorizontalDot;
}

Maths::Vector3 RotateHorizontalToward(
    const Maths::Vector3& currentHorizontal,
    const Maths::Vector3& candidateHorizontal)
{
    const float horizontalDot = ClampUnit(Maths::Vector3::Dot(currentHorizontal, candidateHorizontal));
    const float angle = std::acos(horizontalDot);
    if (angle <= kOppositeViewYawStepRadians)
        return candidateHorizontal;

    float crossY =
        currentHorizontal.z * candidateHorizontal.x -
        currentHorizontal.x * candidateHorizontal.z;
    if (std::abs(crossY) <= 0.0001f)
        crossY = 1.0f;

    const float yaw = kOppositeViewYawStepRadians * (crossY >= 0.0f ? 1.0f : -1.0f);
    const float cosYaw = std::cos(yaw);
    const float sinYaw = std::sin(yaw);
    return Maths::Vector3::Normalize({
        currentHorizontal.x * cosYaw + currentHorizontal.z * sinYaw,
        0.0f,
        -currentHorizontal.x * sinYaw + currentHorizontal.z * cosYaw
    });
}

SceneViewCameraTransform BuildOrbitCameraTransform(
    const Maths::Vector3& orbitTarget,
    const Maths::Vector3& forward,
    const float cameraLength)
{
    const Maths::Vector3 nextPosition = orbitTarget - forward * cameraLength;
    return {
        nextPosition,
        Maths::Quaternion::LookAt(forward, Maths::Vector3::Up)
    };
}

Maths::Vector3 StepForwardToward(
    const Maths::Vector3& currentForward,
    const Maths::Vector3& targetForward)
{
    const Maths::Vector3 nextForward = Maths::Vector3::Normalize(
        currentForward + (targetForward - currentForward) * kViewGizmoTargetStepBlend);

    if (Maths::Vector3::Length(nextForward) <= kMinViewGizmoOrbitLength)
        return currentForward;

    return ApplyPitchLimit(nextForward, currentForward);
}
}

SceneViewGizmoOperation ToImGuizmoOperation(const EGizmoOperation p_operation)
{
    switch (p_operation)
    {
    case EGizmoOperation::ROTATE:
        return SceneViewGizmoOperation::Rotate;
    case EGizmoOperation::SCALE:
        return SceneViewGizmoOperation::Scale;
    case EGizmoOperation::TRANSLATE:
    default:
        return SceneViewGizmoOperation::Translate;
    }
}

SceneViewGizmoPivot ToggleGizmoPivot(const SceneViewGizmoPivot p_pivot)
{
    return p_pivot == SceneViewGizmoPivot::Pivot
        ? SceneViewGizmoPivot::Center
        : SceneViewGizmoPivot::Pivot;
}

SceneViewGizmoSpace ToggleGizmoSpace(const SceneViewGizmoSpace p_space)
{
    return p_space == SceneViewGizmoSpace::Global
        ? SceneViewGizmoSpace::Local
        : SceneViewGizmoSpace::Global;
}

float GetSnapValue(const EGizmoOperation p_operation)
{
    switch (p_operation)
    {
    case EGizmoOperation::ROTATE:
        return Settings::EditorSettings::GetSceneToolSettingsObject().rotationSnapUnit;
    case EGizmoOperation::SCALE:
        return Settings::EditorSettings::GetSceneToolSettingsObject().scalingSnapUnit;
    case EGizmoOperation::TRANSLATE:
    default:
        return Settings::EditorSettings::GetSceneToolSettingsObject().translationSnapUnit;
    }
}

bool IsSnapModifierActive(
    const Windowing::Inputs::EKeyState p_leftControl,
    const Windowing::Inputs::EKeyState p_rightControl)
{
    return p_leftControl == Windowing::Inputs::EKeyState::KEY_DOWN ||
        p_rightControl == Windowing::Inputs::EKeyState::KEY_DOWN;
}

bool ShouldSuppressScenePicking(const SceneViewGizmoInteraction& p_interaction)
{
    return p_interaction.isHovered || p_interaction.isUsing ||
        p_interaction.isViewHovered || p_interaction.isViewUsing;
}

SceneViewGizmoMatrix ToImGuizmoMatrix(const Maths::Matrix4& p_matrix)
{
    SceneViewGizmoMatrix result {};
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            result[static_cast<size_t>(column * 4 + row)] =
                p_matrix.data[static_cast<size_t>(row * 4 + column)];
        }
    }
    return result;
}

Maths::Matrix4 FromImGuizmoMatrix(const SceneViewGizmoMatrix& p_matrix)
{
    Maths::Matrix4 result;
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            result.data[static_cast<size_t>(row * 4 + column)] =
                p_matrix[static_cast<size_t>(column * 4 + row)];
        }
    }
    return result;
}

SceneViewCameraTransform GetCameraTransformFromViewMatrix(const SceneViewGizmoMatrix& p_viewMatrix)
{
    const Maths::Matrix4 cameraWorldMatrix = Maths::Matrix4::Inverse(FromImGuizmoMatrix(p_viewMatrix));
    const Maths::Vector3 position {
        cameraWorldMatrix.data[3],
        cameraWorldMatrix.data[7],
        cameraWorldMatrix.data[11]
    };
    const Maths::Vector3 right = -Maths::Vector3::Normalize({
        cameraWorldMatrix.data[0],
        cameraWorldMatrix.data[4],
        cameraWorldMatrix.data[8]
    });
    const Maths::Vector3 up = Maths::Vector3::Normalize({
        cameraWorldMatrix.data[1],
        cameraWorldMatrix.data[5],
        cameraWorldMatrix.data[9]
    });
    const Maths::Vector3 forward = -Maths::Vector3::Normalize({
        cameraWorldMatrix.data[2],
        cameraWorldMatrix.data[6],
        cameraWorldMatrix.data[10]
    });
    const Maths::Matrix4 rotationMatrix(
        right.x, up.x, forward.x, 0.0f,
        right.y, up.y, forward.y, 0.0f,
        right.z, up.z, forward.z, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f);

    return {position, Maths::Quaternion(rotationMatrix)};
}

Maths::Vector3 GetCameraForwardFromImGuizmoViewTargetDirection(const Maths::Vector3& p_targetDirection)
{
    return Maths::Vector3::Normalize(-p_targetDirection);
}

bool ShouldApplyViewGizmoCameraTransform(
    const bool p_isViewUsing,
    const bool p_isLeftMouseDown,
    const Maths::Vector2& p_mouseDelta)
{
    (void)p_mouseDelta;
    return p_isViewUsing && !p_isLeftMouseDown;
}

bool ShouldCancelViewGizmoCameraTransform(
    const bool p_isCameraControlActive,
    const bool p_isViewUsing)
{
    return p_isCameraControlActive && p_isViewUsing;
}

bool IsViewGizmoOppositeDirectionJump(
    const Maths::Quaternion& p_currentRotation,
    const Maths::Quaternion& p_candidateRotation)
{
    const Maths::Vector3 currentForward =
        Maths::Vector3::Normalize(p_currentRotation * Maths::Vector3::Forward);
    const Maths::Vector3 candidateForward =
        Maths::Vector3::Normalize(p_candidateRotation * Maths::Vector3::Forward);

    return Maths::Vector3::Dot(currentForward, candidateForward) < -0.999f;
}

SceneViewCameraTransform StabilizeViewGizmoCameraTransform(
    const SceneViewCameraTransform& p_currentTransform,
    const SceneViewCameraTransform& p_candidateTransform,
    const float p_cameraLength,
    const Maths::Vector3& p_orbitFocus,
    const Maths::Vector3* p_targetForward)
{
    const Maths::Vector3 candidateForward =
        Maths::Vector3::Normalize(p_candidateTransform.rotation * Maths::Vector3::Forward);
    const Maths::Vector3 routingForward =
        p_targetForward != nullptr
            ? Maths::Vector3::Normalize(*p_targetForward)
            : candidateForward;
    const bool oppositeDirection =
        p_targetForward != nullptr
            ? Maths::Vector3::Dot(
                Maths::Vector3::Normalize(p_currentTransform.rotation * Maths::Vector3::Forward),
                routingForward) < -0.999f
            : IsViewGizmoOppositeDirectionJump(p_currentTransform.rotation, p_candidateTransform.rotation);
    const Maths::Vector3 currentForward =
        Maths::Vector3::Normalize(p_currentTransform.rotation * Maths::Vector3::Forward);
    const Maths::Vector3 orbitTarget = p_orbitFocus;

    if (p_targetForward != nullptr &&
        !oppositeDirection &&
        !ShouldRouteViewGizmoAroundWorldY(currentForward, routingForward))
    {
        return BuildOrbitCameraTransform(
            orbitTarget,
            StepForwardToward(currentForward, routingForward),
            p_cameraLength);
    }

    if (!oppositeDirection && !ShouldRouteViewGizmoAroundWorldY(currentForward, routingForward))
    {
        const Maths::Vector3 stableForward = ApplyPitchLimit(candidateForward, currentForward);
        return BuildOrbitCameraTransform(orbitTarget, stableForward, p_cameraLength);
    }

    const Maths::Vector3 currentRight = p_currentTransform.rotation * Maths::Vector3::Right;
    const Maths::Vector3 currentHorizontal = GetHorizontalDirection(currentForward, currentRight);
    const Maths::Vector3 candidateHorizontal = GetHorizontalDirection(routingForward, -currentHorizontal);
    const Maths::Vector3 nextHorizontal = RotateHorizontalToward(currentHorizontal, candidateHorizontal);
    const float y = std::clamp(currentForward.y, -kViewGizmoMaxPitchSin, kViewGizmoMaxPitchSin);
    const float horizontalLength = std::sqrt(std::max(0.0f, 1.0f - y * y));
    const Maths::Vector3 nextForward = Maths::Vector3::Normalize({
        nextHorizontal.x * horizontalLength,
        y,
        nextHorizontal.z * horizontalLength
    });

    return BuildOrbitCameraTransform(orbitTarget, nextForward, p_cameraLength);
}

Maths::Vector3 ExtractPosition(const Maths::Matrix4& p_matrix)
{
    return {p_matrix.data[3], p_matrix.data[7], p_matrix.data[11]};
}

Maths::Vector3 ExtractScale(const Maths::Matrix4& p_matrix)
{
    return {
        Maths::Vector3::Length({p_matrix.data[0], p_matrix.data[4], p_matrix.data[8]}),
        Maths::Vector3::Length({p_matrix.data[1], p_matrix.data[5], p_matrix.data[9]}),
        Maths::Vector3::Length({p_matrix.data[2], p_matrix.data[6], p_matrix.data[10]}),
    };
}

Maths::Quaternion ExtractRotation(const Maths::Matrix4& p_matrix)
{
    Maths::Vector3 right {p_matrix.data[0], p_matrix.data[4], p_matrix.data[8]};
    Maths::Vector3 up {p_matrix.data[1], p_matrix.data[5], p_matrix.data[9]};
    Maths::Vector3 forward {p_matrix.data[2], p_matrix.data[6], p_matrix.data[10]};

    right = Maths::Vector3::Normalize(right);
    up = up - right * Maths::Vector3::Dot(up, right);
    up = Maths::Vector3::Normalize(up);
    forward = Maths::Vector3::Cross(right, up);

    if (Maths::Vector3::Dot(forward, Maths::Vector3::Normalize({p_matrix.data[2], p_matrix.data[6], p_matrix.data[10]})) < 0.0f)
        forward = -forward;

    const Maths::Matrix3 rotationMatrix(
        right.x, up.x, forward.x,
        right.y, up.y, forward.y,
        right.z, up.z, forward.z);

    return Maths::Quaternion(rotationMatrix);
}

bool ExpandWorldBounds(
    WorldBounds& bounds,
    bool& hasBounds,
    const Maths::Vector3& center,
    const Maths::Vector3& extents)
{
    const Maths::Vector3 min = center - extents;
    const Maths::Vector3 max = center + extents;

    if (!hasBounds)
    {
        bounds = {min, max};
        hasBounds = true;
        return true;
    }

    bounds.min.x = std::min(bounds.min.x, min.x);
    bounds.min.y = std::min(bounds.min.y, min.y);
    bounds.min.z = std::min(bounds.min.z, min.z);
    bounds.max.x = std::max(bounds.max.x, max.x);
    bounds.max.y = std::max(bounds.max.y, max.y);
    bounds.max.z = std::max(bounds.max.z, max.z);
    return true;
}

bool AccumulateGameObjectWorldBounds(const Engine::GameObject& actor, WorldBounds& bounds, bool& hasBounds)
{
    const auto* transform = actor.GetTransform();
    if (transform != nullptr)
    {
        if (const auto* meshRenderer = actor.GetComponent<Engine::Components::MeshRenderer>())
        {
            bool hasRenderableBounds = false;
            Maths::Vector3 localCenter = Maths::Vector3::Zero;
            float radius = 0.0f;

            if (meshRenderer->GetFrustumBehaviour() == Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_CUSTOM)
            {
                const auto& customBounds = meshRenderer->GetCustomBoundingSphere();
                localCenter = customBounds.position;
                radius = customBounds.radius;
                hasRenderableBounds = true;
            }
            else if (auto* meshFilter = actor.GetComponent<Engine::Components::MeshFilter>();
                meshFilter != nullptr)
            {
                if (auto* mesh = meshFilter->ResolveMesh())
                {
                    const auto& modelBounds = mesh->GetBoundingSphere();
                    localCenter = modelBounds.position;
                    radius = modelBounds.radius;
                    hasRenderableBounds = true;
                }
            }

            if (hasRenderableBounds)
            {
                const Maths::Vector3 worldCenter =
                    transform->GetWorldMatrix() * Maths::Vector4(localCenter, 1.0f);
                const auto& scale = transform->GetWorldScale();
                const float maxScale = std::max({std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)});
                ExpandWorldBounds(bounds, hasBounds, worldCenter, Maths::Vector3::One * (radius * maxScale));
            }
        }
    }

    for (const auto* child : const_cast<Engine::GameObject&>(actor).GetChildren())
    {
        if (child != nullptr)
            AccumulateGameObjectWorldBounds(*child, bounds, hasBounds);
    }

    return hasBounds;
}

void RestoreGameObjectCenterPivotPosition(
    Engine::GameObject& actor,
    const Maths::Vector3& targetPivotPosition,
    const SceneViewGizmoPivot pivot)
{
    if (pivot != SceneViewGizmoPivot::Center)
        return;

    auto* transform = actor.GetTransform();
    if (transform == nullptr)
        return;

    const Maths::Vector3 currentPivotPosition = GetGameObjectGizmoPivotPosition(actor, pivot);
    transform->SetWorldPosition(transform->GetWorldPosition() + (targetPivotPosition - currentPivotPosition));
}

SceneViewGizmoMatrix GetGameObjectWorldGizmoMatrix(const Engine::GameObject& p_actor)
{
    const auto* transform = p_actor.GetTransform();
    return GetGameObjectWorldGizmoMatrix(p_actor, SceneViewGizmoPivot::Pivot);
}

Maths::Vector3 GetGameObjectGizmoPivotPosition(
    const Engine::GameObject& p_actor,
    const SceneViewGizmoPivot p_pivot)
{
    const auto* transform = p_actor.GetTransform();
    if (transform == nullptr)
        return Maths::Vector3::Zero;

    if (p_pivot == SceneViewGizmoPivot::Center)
    {
        WorldBounds bounds {};
        bool hasBounds = false;
        if (AccumulateGameObjectWorldBounds(p_actor, bounds, hasBounds))
            return (bounds.min + bounds.max) * 0.5f;
    }

    return transform->GetWorldPosition();
}

SceneViewViewGizmoRect GetSceneViewViewGizmoRect(
    const Maths::Vector2& p_viewMin,
    const Maths::Vector2& p_viewMax)
{
    const float viewWidth = std::max(0.0f, p_viewMax.x - p_viewMin.x);
    const float viewHeight = std::max(0.0f, p_viewMax.y - p_viewMin.y);
    const float size = std::min(kViewGizmoSize, std::min(viewWidth, viewHeight));

    return {
        {p_viewMax.x - size - kViewGizmoMargin, p_viewMin.y + kViewGizmoMargin},
        {size, size}
    };
}

SceneViewGizmoMatrix GetGameObjectWorldGizmoMatrix(
    const Engine::GameObject& p_actor,
    const SceneViewGizmoPivot p_pivot)
{
    const auto* transform = p_actor.GetTransform();
    if (transform == nullptr)
        return ToImGuizmoMatrix(Maths::Matrix4::Identity);

    auto matrix = transform->GetWorldMatrix();
    const auto pivotPosition = GetGameObjectGizmoPivotPosition(p_actor, p_pivot);
    matrix.data[3] = pivotPosition.x;
    matrix.data[7] = pivotPosition.y;
    matrix.data[11] = pivotPosition.z;
    return ToImGuizmoMatrix(matrix);
}

void ApplyGameObjectWorldGizmoMatrix(
    Engine::GameObject& p_actor,
    const SceneViewGizmoMatrix& p_matrix,
    const EGizmoOperation p_operation,
    const SceneViewGizmoPivot p_pivot)
{
    auto* transform = p_actor.GetTransform();
    if (transform == nullptr)
        return;

    const Maths::Vector3 previousPivotPosition = GetGameObjectGizmoPivotPosition(p_actor, p_pivot);
    const Maths::Matrix4 worldMatrix = FromImGuizmoMatrix(p_matrix);

    switch (p_operation)
    {
    case EGizmoOperation::ROTATE:
        transform->SetWorldRotation(ExtractRotation(worldMatrix));
        RestoreGameObjectCenterPivotPosition(p_actor, ExtractPosition(worldMatrix), p_pivot);
        break;
    case EGizmoOperation::SCALE:
        transform->SetWorldScale(ExtractScale(worldMatrix));
        RestoreGameObjectCenterPivotPosition(p_actor, ExtractPosition(worldMatrix), p_pivot);
        break;
    case EGizmoOperation::TRANSLATE:
    default:
        transform->SetWorldPosition(transform->GetWorldPosition() + (ExtractPosition(worldMatrix) - previousPivotPosition));
        break;
    }
}
}
