#pragma once

#include <optional>
#include <span>
#include <vector>
#include <Math/Matrix4.h>
#include <Math/Vector2.h>
#include <Math/Vector3.h>
#include "Core/SceneViewImGuizmo.h"
#include "Inspector/InspectorTypes.h"

namespace NLS::Engine { class GameObject; namespace SceneSystem { class Scene; } }

namespace NLS::Editor::Core
{
[[nodiscard]] Inspector::InspectorTargetSet FilterSceneSelection(const Inspector::InspectorTargetSet& selection,
    const Engine::SceneSystem::Scene& scene);
struct SceneSelectionBounds { Maths::Vector3 min, max; };
struct SceneSelectionRect
{
    Maths::Vector2 min, max;
    [[nodiscard]] bool Intersects(const SceneSelectionRect& other) const;
};
struct SceneSelectionDrag
{
    Maths::Vector2 start, current;
    bool marquee = false;
    void Update(const Maths::Vector2& point, float threshold);
};
struct SceneSelectionTransform
{
    Maths::Vector3 worldPosition;
    Maths::Quaternion worldRotation;
    Maths::Vector3 localScale {1,1,1};
};
[[nodiscard]] Maths::Matrix4 BuildSceneSelectionGizmoMatrix(std::span<Engine::GameObject* const> objects,
    const Engine::GameObject& active, SceneViewGizmoPivot pivot, SceneViewGizmoSpace space);
[[nodiscard]] SceneSelectionTransform TransformSceneSelection(const SceneSelectionTransform& before,
    const Maths::Matrix4& startHandle, const Maths::Matrix4& currentHandle,
    EGizmoOperation operation, SceneViewGizmoPivot pivot, SceneViewGizmoSpace space);

// Preserve input order, dropping duplicates and descendants of another member.
[[nodiscard]] std::vector<Engine::GameObject*> GetSceneSelectionRoots(
    std::span<Engine::GameObject* const> objects);
[[nodiscard]] std::optional<SceneSelectionBounds> GetSceneSelectionBounds(
    std::span<Engine::GameObject* const> objects, bool includeChildren = true);
[[nodiscard]] std::optional<SceneSelectionRect> ProjectSceneSelectionBounds(
    const SceneSelectionBounds& bounds, const Maths::Matrix4& viewProjection,
    const SceneSelectionRect& viewport);
[[nodiscard]] SceneSelectionRect MakeSceneSelectionRect(const Maths::Vector2& from, const Maths::Vector2& to);
}
