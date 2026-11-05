#include "Core/SceneSelection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <Engine/GameObject.h>
#include <Engine/Components/MeshFilter.h>
#include <Engine/Components/MeshRenderer.h>
#include <Engine/Components/TransformComponent.h>
#include <Rendering/Resources/Mesh.h>
#include <Math/Vector4.h>
#include "Inspector/NativeReflectionAdapter.h"

namespace NLS::Editor::Core
{
Inspector::InspectorTargetSet FilterSceneSelection(const Inspector::InspectorTargetSet& selection,
    const Engine::SceneSystem::Scene& scene)
{
    auto result = selection;
    std::erase_if(result.targets, [&scene](const auto& target)
    {
        if (target.kind != Inspector::InspectorTargetKind::GameObject) return true;
        auto* object = dynamic_cast<Engine::GameObject*>(Inspector::NativeReflectionAdapter::ResolveObject(target));
        return !object || object->GetScene() != &scene || object->IsEditorTransient();
    });
    result.Normalize();
    return result;
}
namespace
{
float VectorLength(const Maths::Vector3& value)
{ return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z); }
Maths::Vector3 Column(const Maths::Matrix4& matrix, int column)
{ return {matrix.data[column], matrix.data[4 + column], matrix.data[8 + column]}; }
Maths::Quaternion HandleRotation(const Maths::Matrix4& matrix)
{
    auto rotation = Maths::Matrix4::Identity;
    for (int column = 0; column < 3; ++column)
    {
        auto axis = Column(matrix, column);
        const float length = VectorLength(axis);
        if (length <= 1.e-6f) return Maths::Quaternion::Identity;
        axis = axis / length;
        rotation.data[column] = axis.x; rotation.data[4 + column] = axis.y; rotation.data[8 + column] = axis.z;
    }
    return Maths::Quaternion::Normalize(Maths::Quaternion(rotation));
}
Maths::Vector3 Product(const Maths::Vector3& a, const Maths::Vector3& b)
{ return {a.x*b.x, a.y*b.y, a.z*b.z}; }
}

Maths::Matrix4 BuildSceneSelectionGizmoMatrix(std::span<Engine::GameObject* const> objects,
    const Engine::GameObject& active, const SceneViewGizmoPivot pivot, const SceneViewGizmoSpace space)
{
    auto position = active.GetTransform()->GetWorldPosition();
    if (pivot == SceneViewGizmoPivot::Center)
    {
        if (auto bounds = GetSceneSelectionBounds(objects)) position = (bounds->min + bounds->max) * .5f;
        else
        {
            position = Maths::Vector3::Zero; size_t count = 0;
            for (auto* object : objects) if (object && object->GetTransform()) { position += object->GetTransform()->GetWorldPosition(); ++count; }
            if (count) position = position / static_cast<float>(count);
        }
    }
    const auto rotation = space == SceneViewGizmoSpace::Local ? active.GetTransform()->GetWorldRotation() : Maths::Quaternion::Identity;
    return Maths::Matrix4::Translation(position) * Maths::Quaternion::ToMatrix4(rotation);
}

SceneSelectionTransform TransformSceneSelection(const SceneSelectionTransform& before,
    const Maths::Matrix4& startHandle, const Maths::Matrix4& currentHandle,
    const EGizmoOperation operation, const SceneViewGizmoPivot pivot, const SceneViewGizmoSpace space)
{
    auto result = before;
    const Maths::Vector3 center{startHandle.data[3], startHandle.data[7], startHandle.data[11]};
    const Maths::Vector3 current{currentHandle.data[3], currentHandle.data[7], currentHandle.data[11]};
    const auto basis = HandleRotation(startHandle);
    if (operation == EGizmoOperation::TRANSLATE) result.worldPosition += current - center;
    else if (operation == EGizmoOperation::ROTATE)
    {
        const auto delta = HandleRotation(currentHandle) * Maths::Quaternion::Inverse(basis);
        result.worldRotation = pivot == SceneViewGizmoPivot::Pivot && space == SceneViewGizmoSpace::Local
            ? before.worldRotation * (Maths::Quaternion::Inverse(basis) * HandleRotation(currentHandle))
            : delta * before.worldRotation;
        if (pivot == SceneViewGizmoPivot::Center) result.worldPosition = center + delta * (before.worldPosition - center);
    }
    else if (operation == EGizmoOperation::SCALE)
    {
        Maths::Vector3 ratio;
        for (int axis = 0; axis < 3; ++axis)
            ratio[axis] = VectorLength(Column(currentHandle, axis)) / (std::max)(1.e-6f, VectorLength(Column(startHandle, axis)));
        if (pivot == SceneViewGizmoPivot::Center)
            result.worldPosition = center + basis * Product(ratio, Maths::Quaternion::Inverse(basis) * (before.worldPosition - center));
        if (pivot == SceneViewGizmoPivot::Pivot && space == SceneViewGizmoSpace::Local)
            result.localScale = Product(before.localScale, ratio);
        else
            for (int axis = 0; axis < 3; ++axis)
            {
                Maths::Vector3 unit{}; unit[axis] = 1;
                const auto local = Maths::Quaternion::Inverse(basis) * (before.worldRotation * unit);
                result.localScale[axis] = before.localScale[axis] * VectorLength(Product(ratio, local));
            }
    }
    return result;
}

void SceneSelectionDrag::Update(const Maths::Vector2& point, const float threshold)
{
    current = point;
    const auto delta = current - start;
    marquee |= delta.x * delta.x + delta.y * delta.y > threshold * threshold;
}
bool SceneSelectionRect::Intersects(const SceneSelectionRect& other) const
{
    return min.x <= max.x && min.y <= max.y && other.min.x <= other.max.x && other.min.y <= other.max.y &&
        min.x <= other.max.x && max.x >= other.min.x && min.y <= other.max.y && max.y >= other.min.y;
}

SceneSelectionRect MakeSceneSelectionRect(const Maths::Vector2& from, const Maths::Vector2& to)
{
    return {{(std::min)(from.x, to.x), (std::min)(from.y, to.y)},
        {(std::max)(from.x, to.x), (std::max)(from.y, to.y)}};
}

std::vector<Engine::GameObject*> GetSceneSelectionRoots(std::span<Engine::GameObject* const> objects)
{
    std::unordered_set<Engine::GameObject*> members(objects.begin(), objects.end()), visited;
    std::vector<Engine::GameObject*> roots;
    for (auto* object : objects)
    {
        if (!object || !visited.insert(object).second) continue;
        bool covered = false;
        for (auto* parent = object->GetParent(); parent; parent = parent->GetParent())
            if (members.contains(parent)) { covered = true; break; }
        if (!covered) roots.push_back(object);
    }
    return roots;
}

std::optional<SceneSelectionBounds> GetSceneSelectionBounds(
    std::span<Engine::GameObject* const> objects, const bool includeChildren)
{
    std::optional<SceneSelectionBounds> bounds;
    std::unordered_set<Engine::GameObject*> visited;
    std::unordered_map<Engine::GameObject*, bool> geometryByObject;
    const auto expand = [&bounds](const Maths::Vector3& point)
    {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) return;
        if (!bounds) { bounds = SceneSelectionBounds{point, point}; return; }
        for (int axis = 0; axis < 3; ++axis)
        {
            bounds->min[axis] = (std::min)(bounds->min[axis], point[axis]);
            bounds->max[axis] = (std::max)(bounds->max[axis], point[axis]);
        }
    };
    const auto collect = [&](auto&& self, Engine::GameObject* object) -> bool
    {
        if (!object || object->IsEditorTransient()) return false;
        if (!visited.insert(object).second) return geometryByObject[object];
        bool hasGeometry = false;
        if (auto* transform = object->GetTransform())
            if (auto* renderer = object->GetComponent<Engine::Components::MeshRenderer>(); renderer && renderer->IsSelfEnabled())
                if (auto* filter = object->GetComponent<Engine::Components::MeshFilter>())
                    if (auto* mesh = filter->ResolveMesh())
                    {
                        hasGeometry = true;
                        const auto& local = mesh->GetBounds();
                        for (int corner = 0; corner < 8; ++corner)
                        {
                            const auto p = local.center + Maths::Vector3{
                                local.size.x * ((corner & 1) ? .5f : -.5f),
                                local.size.y * ((corner & 2) ? .5f : -.5f),
                                local.size.z * ((corner & 4) ? .5f : -.5f)};
                            const auto world = transform->GetWorldMatrix() * Maths::Vector4(p.x, p.y, p.z, 1);
                            expand({world.x, world.y, world.z});
                        }
                    }
        if (includeChildren)
            for (auto* child : object->GetChildren()) hasGeometry = self(self, child) || hasGeometry;
        geometryByObject[object] = hasGeometry;
        return hasGeometry;
    };
    if (includeChildren)
    {
        // Empty nodes, cameras and lights still contribute their positions to a
        // selection's center, even when another selected object has geometry.
        for (auto* object : GetSceneSelectionRoots(objects)) collect(collect, object);
        for (auto* object : objects)
            if (object && !object->IsEditorTransient() && !geometryByObject[object] && object->GetTransform())
                expand(object->GetTransform()->GetWorldPosition());
    }
    else
        for (auto* object : objects) collect(collect, object);
    return bounds;
}

std::optional<SceneSelectionRect> ProjectSceneSelectionBounds(
    const SceneSelectionBounds& bounds, const Maths::Matrix4& viewProjection,
    const SceneSelectionRect& viewport)
{
    if (viewport.max.x <= viewport.min.x || viewport.max.y <= viewport.min.y) return std::nullopt;
    std::array<Maths::Vector4, 8> corners;
    for (int i = 0; i < 8; ++i)
        corners[i] = viewProjection * Maths::Vector4(
            (i & 1) ? bounds.max.x : bounds.min.x, (i & 2) ? bounds.max.y : bounds.min.y,
            (i & 4) ? bounds.max.z : bounds.min.z, 1);
    std::optional<SceneSelectionRect> result;
    const auto project = [&](const Maths::Vector4& point)
    {
        if (!std::isfinite(point.w) || point.w <= 1.e-6f) return;
        const Maths::Vector2 screen{
            viewport.min.x + (point.x / point.w + 1) * .5f * (viewport.max.x - viewport.min.x),
            viewport.min.y + (1 - point.y / point.w) * .5f * (viewport.max.y - viewport.min.y)};
        if (!std::isfinite(screen.x) || !std::isfinite(screen.y)) return;
        if (!result) result = SceneSelectionRect{screen, screen};
        else
        {
            result->min.x = (std::min)(result->min.x, screen.x); result->min.y = (std::min)(result->min.y, screen.y);
            result->max.x = (std::max)(result->max.x, screen.x); result->max.y = (std::max)(result->max.y, screen.y);
        }
    };
    // Clip the twelve box edges in homogeneous coordinates before dividing by
    // w, including boxes crossing the near plane or surrounding the camera.
    for (int i = 0; i < 8; ++i)
        for (int bit : {1, 2, 4})
        {
            if (i & bit) continue;
            auto from = corners[i], to = corners[i | bit];
            bool valid = true;
            for (int plane = 0; plane < 3; ++plane)
            {
                const auto distance = [plane](const Maths::Vector4& p)
                { return plane == 0 ? p.w - 1.e-5f : plane == 1 ? p.z + p.w : p.w - p.z; };
                const float a = distance(from), b = distance(to);
                if (!std::isfinite(a) || !std::isfinite(b) || (a < 0 && b < 0)) { valid = false; break; }
                if ((a < 0) != (b < 0))
                {
                    const auto intersection = from + (to - from) * (a / (a - b));
                    if (a < 0) from = intersection; else to = intersection;
                }
            }
            if (valid) { project(from); project(to); }
        }
    if (!result || !result->Intersects(viewport)) return std::nullopt;
    result->min.x = (std::max)(result->min.x, viewport.min.x); result->min.y = (std::max)(result->min.y, viewport.min.y);
    result->max.x = (std::min)(result->max.x, viewport.max.x); result->max.y = (std::min)(result->max.y, viewport.max.y);
    return result;
}
}
