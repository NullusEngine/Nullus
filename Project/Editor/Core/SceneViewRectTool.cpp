#include "Core/SceneViewRectTool.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Components/TransformComponent.h"
#include "GameObject.h"
#include "Math/Vector4.h"
#include "Rendering/Resources/Mesh.h"
#include "ImGui/imgui.h"

namespace NLS::Editor::Core
{
namespace
{
constexpr float kEpsilon = 1.0e-6f;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kMinimumScaleFactor = 1.0e-4f;
constexpr std::array<RectToolHandle, 4> kCorners {RectToolHandle::BottomLeft, RectToolHandle::BottomRight,
    RectToolHandle::TopRight, RectToolHandle::TopLeft};
constexpr std::array<RectToolHandle, 4> kEdges {RectToolHandle::Bottom, RectToolHandle::Right,
    RectToolHandle::Top, RectToolHandle::Left};
constexpr std::array<RectToolHandle, 4> kRotations {RectToolHandle::RotateBottomLeft, RectToolHandle::RotateBottomRight,
    RectToolHandle::RotateTopRight, RectToolHandle::RotateTopLeft};

bool Finite(const Maths::Vector3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool Finite(const Maths::Matrix4& m)
{
    for (float value : m.data) if (!std::isfinite(value)) return false;
    return true;
}
std::optional<Maths::Matrix4> TryInverse(const Maths::Matrix4& matrix)
{
    if (!Finite(matrix)) return std::nullopt;
    // Matrix4::Inverse falls back to identity for small determinants. Ordinary
    // orthographic projections and centimeter parents need their true inverse.
    double rows[4][8] {};
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column) rows[row][column] = matrix.data[row * 4 + column];
        rows[row][row + 4] = 1.0;
    }
    for (int column = 0; column < 4; ++column)
    {
        int pivot = column;
        for (int row = column + 1; row < 4; ++row)
            if (std::abs(rows[row][column]) > std::abs(rows[pivot][column])) pivot = row;
        const double divisor = rows[pivot][column];
        if (!std::isfinite(divisor) || divisor == 0.0) return std::nullopt;
        for (int entry = 0; entry < 8; ++entry) std::swap(rows[pivot][entry], rows[column][entry]);
        for (double& value : rows[column]) value /= divisor;
        for (int row = 0; row < 4; ++row)
        {
            if (row == column) continue;
            const double factor = rows[row][column];
            for (int entry = 0; entry < 8; ++entry) rows[row][entry] -= factor * rows[column][entry];
        }
    }
    Maths::Matrix4 result;
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) result.data[row * 4 + column] = static_cast<float>(rows[row][column + 4]);
    return Finite(result) ? std::optional(result) : std::nullopt;
}
Maths::Vector3 Point(const Maths::Matrix4& m, const Maths::Vector3& v)
{
    const auto p = m * Maths::Vector4(v.x, v.y, v.z, 1.0f);
    return {p.x, p.y, p.z};
}
Maths::Vector3 Direction(const Maths::Matrix4& m, const Maths::Vector3& v)
{
    const auto p = m * Maths::Vector4(v.x, v.y, v.z, 0.0f);
    return {p.x, p.y, p.z};
}
Maths::Vector3 Axis(const int index)
{
    Maths::Vector3 axis {};
    axis[index] = 1.0f;
    return axis;
}
Maths::Matrix4 World(const RectToolTarget& target, const RectToolTransform& transform)
{
    return target.parentWorld * Maths::Matrix4::Translation(transform.localPosition) *
        Maths::Quaternion::ToMatrix4(transform.localRotation) * Maths::Matrix4::Scaling(transform.localScale);
}
float LengthSquared(const Maths::Vector2& v) { return v.x * v.x + v.y * v.y; }
float Cross2(const Maths::Vector2& a, const Maths::Vector2& b) { return a.x * b.y - a.y * b.x; }
float Snap(const float value, const float step)
{
    return std::isfinite(step) && step > kEpsilon ? std::round(value / step) * step : value;
}
std::optional<Maths::Vector2> Coordinates(const Maths::Vector3& delta,
    const Maths::Vector3& u, const Maths::Vector3& v)
{
    const float uu = Maths::Vector3::Dot(u, u), uv = Maths::Vector3::Dot(u, v), vv = Maths::Vector3::Dot(v, v);
    const float determinant = uu * vv - uv * uv;
    if (!std::isfinite(determinant) || determinant <= kEpsilon * uu * vv || uu <= 0.0f || vv <= 0.0f)
        return std::nullopt;
    const float du = Maths::Vector3::Dot(delta, u), dv = Maths::Vector3::Dot(delta, v);
    return Maths::Vector2 {(du * vv - dv * uv) / determinant, (dv * uu - du * uv) / determinant};
}
std::optional<Maths::Vector2> Project(const RectToolView& view, const Maths::Vector3& point)
{
    const auto clip = view.viewProjection * Maths::Vector4(point.x, point.y, point.z, 1.0f);
    // Reject a rectangle crossing the near plane rather than drawing enormous/inverted handles.
    if (!std::isfinite(clip.w) || clip.w <= kEpsilon || clip.z < -clip.w || clip.z > clip.w)
        return std::nullopt;
    const Maths::Vector2 screen {view.min.x + (clip.x / clip.w + 1.0f) * 0.5f * (view.max.x - view.min.x),
        view.min.y + (1.0f - clip.y / clip.w) * 0.5f * (view.max.y - view.min.y)};
    return std::isfinite(screen.x) && std::isfinite(screen.y) ? std::optional(screen) : std::nullopt;
}
std::optional<Maths::Vector3> MouseOnPlane(const RectToolFrame& frame, const Maths::Vector2& mouse)
{
    if (!std::isfinite(mouse.x) || !std::isfinite(mouse.y)) return std::nullopt;
    const float x = (mouse.x - frame.view.min.x) / (frame.view.max.x - frame.view.min.x) * 2.0f - 1.0f;
    const float y = 1.0f - (mouse.y - frame.view.min.y) / (frame.view.max.y - frame.view.min.y) * 2.0f;
    const auto unproject = [&](const float z) -> std::optional<Maths::Vector3>
    {
        const auto p = frame.inverseViewProjection * Maths::Vector4(x, y, z, 1.0f);
        if (!std::isfinite(p.w) || std::abs(p.w) <= kEpsilon) return std::nullopt;
        const Maths::Vector3 result {p.x / p.w, p.y / p.w, p.z / p.w};
        return Finite(result) ? std::optional(result) : std::nullopt;
    };
    const auto nearPoint = unproject(0.0f), farPoint = unproject(0.5f);
    if (!nearPoint || !farPoint) return std::nullopt;
    const auto direction = *farPoint - *nearPoint;
    const float denominator = Maths::Vector3::Dot(direction, frame.normal);
    if (std::abs(denominator) <= kEpsilon || !std::isfinite(denominator)) return std::nullopt;
    const auto result = *nearPoint + direction * (Maths::Vector3::Dot(frame.origin - *nearPoint, frame.normal) / denominator);
    return Finite(result) ? std::optional(result) : std::nullopt;
}
std::optional<float> RotationAngle(const RectToolFrame& frame, const Maths::Vector3& point)
{
    auto inputPivot = frame.pivotWorld;
    if (!frame.view.is2D)
    {
        auto localPivot = frame.pivot == SceneViewGizmoPivot::Center
            ? (frame.target.boundsMin + frame.target.boundsMax) * 0.5f : Maths::Vector3::Zero;
        const int perpendicular = 3 - frame.axisU - frame.axisV;
        localPivot[perpendicular] = frame.localCorners[0][perpendicular];
        inputPivot = Point(frame.world, localPivot);
    }
    const auto delta = point - inputPivot;
    const auto coordinates = Coordinates(delta, frame.rotationBasisU, frame.rotationBasisV);
    if (!coordinates || LengthSquared(*coordinates) <= kEpsilon) return std::nullopt;
    return std::atan2(coordinates->y, coordinates->x);
}
Maths::Quaternion AxisAngle(const Maths::Vector3& axis, const float angle)
{
    const float sine = std::sin(angle * 0.5f);
    return {axis.x * sine, axis.y * sine, axis.z * sine, std::cos(angle * 0.5f)};
}
void AccumulateBounds(const Engine::GameObject& object, const Maths::Matrix4& inverseRoot,
    Maths::Vector3& min, Maths::Vector3& max, bool& found)
{
    if (object.GetTransform() && object.GetComponent<Engine::Components::MeshRenderer>())
        if (auto* filter = object.GetComponent<Engine::Components::MeshFilter>())
            if (auto* mesh = filter->ResolveMesh())
            {
                const auto& bounds = mesh->GetBounds();
                const auto localMatrix = inverseRoot * object.GetTransform()->GetWorldMatrix();
                for (int corner = 0; corner < 8; ++corner)
                {
                    const auto p = Point(localMatrix, bounds.center + Maths::Vector3 {
                        (corner & 1 ? 0.5f : -0.5f) * bounds.size.x,
                        (corner & 2 ? 0.5f : -0.5f) * bounds.size.y,
                        (corner & 4 ? 0.5f : -0.5f) * bounds.size.z});
                    if (!Finite(p)) continue;
                    if (!found) { min = max = p; found = true; }
                    else for (int axis = 0; axis < 3; ++axis)
                    { min[axis] = std::min(min[axis], p[axis]); max[axis] = std::max(max[axis], p[axis]); }
                }
            }
    for (const auto* child : object.GetChildren())
        if (child) AccumulateBounds(*child, inverseRoot, min, max, found);
}
Maths::Vector2 HandleSigns(const RectToolHandle handle)
{
    switch (handle)
    {
    case RectToolHandle::BottomLeft: return {-1, -1};
    case RectToolHandle::BottomRight: return {1, -1};
    case RectToolHandle::TopRight: return {1, 1};
    case RectToolHandle::TopLeft: return {-1, 1};
    case RectToolHandle::Bottom: return {0, -1};
    case RectToolHandle::Right: return {1, 0};
    case RectToolHandle::Top: return {0, 1};
    case RectToolHandle::Left: return {-1, 0};
    default: return {};
    }
}
}

bool operator==(const RectToolTransform& left, const RectToolTransform& right)
{
    return left.localPosition == right.localPosition && left.localScale == right.localScale &&
        left.localRotation.x == right.localRotation.x && left.localRotation.y == right.localRotation.y &&
        left.localRotation.z == right.localRotation.z && left.localRotation.w == right.localRotation.w;
}

RectToolTarget CaptureRectToolTarget(const Engine::GameObject& object)
{
    RectToolTarget target;
    if (const auto* transform = object.GetTransform())
    {
        target.transform = {transform->GetLocalPosition(), transform->GetLocalRotation(), transform->GetLocalScale()};
        if (const auto* parent = object.GetParent(); parent && parent->GetTransform())
            target.parentWorld = parent->GetTransform()->GetWorldMatrix();
        // Compose the authored rotations: matrix decomposition loses reflection signs and introduces shear.
        for (const auto* parent = object.GetParent(); parent; parent = parent->GetParent())
            if (parent->GetTransform())
                target.parentRotation = parent->GetTransform()->GetLocalRotation() * target.parentRotation;
        const auto& world = transform->GetWorldMatrix();
        if (const auto inverse = TryInverse(world))
        {
            bool found = false;
            AccumulateBounds(object, *inverse, target.boundsMin, target.boundsMax, found);
        }
    }
    return target;
}

std::optional<RectToolFrame> BuildRectToolFrame(const RectToolTarget& target, const RectToolView& view,
    const SceneViewGizmoPivot pivot, const std::optional<RectToolPlane> lockedPlane)
{
    if (view.max.x <= view.min.x || view.max.y <= view.min.y || !Finite(target.parentWorld) ||
        !Finite(view.viewProjection) || !Finite(target.transform.localPosition) || !Finite(target.transform.localScale) ||
        !Finite(target.boundsMin) || !Finite(target.boundsMax)) return std::nullopt;
    const auto inverseParent = TryInverse(target.parentWorld), inverseProjection = TryInverse(view.viewProjection);
    if (!inverseParent || !inverseProjection) return std::nullopt;
    RectToolFrame frame;
    frame.target = target; frame.view = view; frame.pivot = pivot;
    frame.world = World(target, target.transform);
    if (!Finite(frame.world)) return std::nullopt;
    frame.inverseParent = *inverseParent;
    frame.inverseViewProjection = *inverseProjection;
    const auto center = (target.boundsMin + target.boundsMax) * 0.5f;
    const auto centerWorld = Point(frame.world, center);
    frame.pivotWorld = Point(frame.world, pivot == SceneViewGizmoPivot::Center ? center : Maths::Vector3::Zero);
    const auto towardCamera = view.orthographic ? -view.cameraForward : view.cameraPosition - centerWorld;
    constexpr std::array<std::array<int, 2>, 3> axes {{{0, 1}, {0, 2}, {1, 2}}};
    int chosen = 0;
    if (!view.is2D && lockedPlane) chosen = static_cast<int>(*lockedPlane);
    else if (!view.is2D)
    {
        float best = -1.0f;
        for (int plane = 0; plane < 3; ++plane)
        {
            const auto normal = Maths::Vector3::Cross(Direction(frame.world, Axis(axes[plane][0])),
                Direction(frame.world, Axis(axes[plane][1])));
            const float length = Maths::Vector3::Length(normal);
            const float score = length > kEpsilon ? std::abs(Maths::Vector3::Dot(normal / length, towardCamera)) : -1.0f;
            if (score > best) { chosen = plane; best = score; }
        }
    }
    frame.plane = static_cast<RectToolPlane>(chosen);
    frame.axisU = axes[chosen][0]; frame.axisV = axes[chosen][1];
    const int perpendicular = 3 - frame.axisU - frame.axisV;
    frame.basisU = Direction(frame.world, Axis(frame.axisU));
    frame.basisV = Direction(frame.world, Axis(frame.axisV));
    const auto rotationWorld = target.parentWorld * Maths::Quaternion::ToMatrix4(target.transform.localRotation);
    frame.rotationBasisU = Direction(rotationWorld, Axis(frame.axisU));
    frame.rotationBasisV = Direction(rotationWorld, Axis(frame.axisV));
    frame.normal = Maths::Vector3::Cross(frame.basisU, frame.basisV);
    const float normalLength = Maths::Vector3::Length(frame.normal);
    if (!std::isfinite(normalLength) || normalLength <= 0.0f) return std::nullopt;
    frame.normal /= normalLength;
    float face = center[perpendicular];
    if (!view.is2D)
        face = Maths::Vector3::Dot(Direction(frame.world, Axis(perpendicular)), towardCamera) >= 0.0f
            ? target.boundsMax[perpendicular] : target.boundsMin[perpendicular];
    if (view.is2D)
    {
        frame.basisU.z = frame.basisV.z = 0.0f;
        frame.rotationBasisU = Maths::Vector3::Right;
        frame.rotationBasisV = Maths::Vector3::Up;
        frame.normal = Maths::Vector3::Forward;
    }
    if (!Coordinates(frame.basisU, frame.basisU, frame.basisV)) return std::nullopt;
    for (int corner = 0; corner < 4; ++corner)
    {
        auto p = center;
        p[frame.axisU] = corner == 1 || corner == 2 ? target.boundsMax[frame.axisU] : target.boundsMin[frame.axisU];
        p[frame.axisV] = corner >= 2 ? target.boundsMax[frame.axisV] : target.boundsMin[frame.axisV];
        p[perpendicular] = face;
        frame.localCorners[corner] = p;
        auto worldPoint = Point(frame.world, p);
        if (view.is2D) worldPoint.z = frame.pivotWorld.z;
        const auto projected = Project(view, worldPoint);
        if (!projected) return std::nullopt;
        frame.corners[corner] = *projected;
        frame.origin += worldPoint * 0.25f;
    }
    const auto screenU = frame.corners[1] - frame.corners[0], screenV = frame.corners[3] - frame.corners[0];
    if (std::abs(Cross2(screenU, screenV)) < 1.0f) return std::nullopt;
    return frame;
}

RectToolHandle HitTestRectTool(const RectToolFrame& frame, const Maths::Vector2& mouse, const float uiScale)
{
    if (!std::isfinite(mouse.x) || !std::isfinite(mouse.y) || mouse.x < frame.view.min.x || mouse.y < frame.view.min.y ||
        mouse.x >= frame.view.max.x || mouse.y >= frame.view.max.y) return RectToolHandle::None;
    const float radius = 6.0f * uiScale;
    for (int i = 0; i < 4; ++i)
        if (LengthSquared(mouse - frame.corners[i]) <= radius * radius) return kCorners[i];
    for (int i = 0; i < 4; ++i)
    {
        const auto a = frame.corners[i], edge = frame.corners[(i + 1) % 4] - a;
        const float length = LengthSquared(edge);
        const auto delta = mouse - a;
        const float t = length > kEpsilon ? std::clamp((delta.x * edge.x + delta.y * edge.y) / length, 0.0f, 1.0f) : 0.0f;
        if (LengthSquared(mouse - (a + edge * t)) <= radius * radius) return kEdges[i];
    }
    bool positive = false, negative = false;
    Maths::Vector2 center {};
    for (int i = 0; i < 4; ++i)
    {
        const float cross = Cross2(frame.corners[(i + 1) % 4] - frame.corners[i], mouse - frame.corners[i]);
        positive |= cross > 0.0f; negative |= cross < 0.0f;
        center += frame.corners[i] * 0.25f;
    }
    if (!(positive && negative)) return RectToolHandle::Move;
    for (int i = 0; i < 4; ++i)
    {
        const auto delta = mouse - frame.corners[i], outward = frame.corners[i] - center;
        const float distance = LengthSquared(delta);
        if (distance >= 8.0f * 8.0f * uiScale * uiScale && distance <= 22.0f * 22.0f * uiScale * uiScale &&
            delta.x * outward.x + delta.y * outward.y > 0.0f) return kRotations[i];
    }
    return RectToolHandle::None;
}

EGizmoOperation GetRectToolHandleOperation(const RectToolHandle handle)
{
    if (handle >= RectToolHandle::RotateBottomLeft) return EGizmoOperation::ROTATE;
    if (handle >= RectToolHandle::BottomLeft) return EGizmoOperation::SCALE;
    return EGizmoOperation::TRANSLATE;
}

std::optional<RectToolDrag> BeginRectToolDrag(const RectToolFrame& frame, const RectToolHandle handle,
    const Maths::Vector2& mouse)
{
    if (handle == RectToolHandle::None) return std::nullopt;
    const auto point = MouseOnPlane(frame, mouse);
    if (!point) return std::nullopt;
    RectToolDrag drag;
    drag.frame = frame; drag.handle = handle; drag.startPoint = *point;
    if (GetRectToolHandleOperation(handle) == EGizmoOperation::ROTATE)
    {
        const auto angle = RotationAngle(frame, *point);
        if (!angle) return std::nullopt;
        drag.lastAngle = *angle;
    }
    return drag;
}

std::optional<RectToolTransform> UpdateRectToolDrag(RectToolDrag& drag, const Maths::Vector2& mouse,
    const RectToolSnap& snap)
{
    const auto& frame = drag.frame;
    auto result = frame.target.transform;
    const auto point = MouseOnPlane(frame, mouse);
    if (!point) return std::nullopt;
    const auto operation = GetRectToolHandleOperation(drag.handle);
    if (operation == EGizmoOperation::TRANSLATE)
    {
        auto delta = *point - drag.startPoint;
        if (snap.enabled)
        {
            const auto u = frame.view.is2D ? Maths::Vector3::Right : Maths::Vector3::Normalize(frame.basisU);
            const auto v = frame.view.is2D ? Maths::Vector3::Up : Maths::Vector3::Normalize(frame.basisV);
            const auto coordinates = Coordinates(delta, u, v);
            if (!coordinates) return std::nullopt;
            delta = u * Snap(coordinates->x, snap.translation) + v * Snap(coordinates->y, snap.translation);
        }
        if (frame.view.is2D) delta.z = 0.0f;
        result.localPosition += Direction(frame.inverseParent, delta);
    }
    else if (operation == EGizmoOperation::SCALE)
    {
        const auto delta = Coordinates(*point - drag.startPoint, frame.basisU, frame.basisV);
        if (!delta) return std::nullopt;
        const auto signs = HandleSigns(drag.handle);
        const auto resize = [&](const int axis, const float sign, const float movement) -> bool
        {
            if (sign == 0.0f) return true;
            const float span = frame.target.boundsMax[axis] - frame.target.boundsMin[axis];
            if (span <= kEpsilon || std::abs(result.localScale[axis]) <= kEpsilon) return false;
            float factor = 1.0f + sign * movement / span;
            if (snap.enabled)
                factor = 1.0f + Snap((factor - 1.0f) * std::abs(result.localScale[axis]), snap.scale) /
                    std::abs(result.localScale[axis]);
            result.localScale[axis] *= std::max(kMinimumScaleFactor, factor);
            return true;
        };
        if (!resize(frame.axisU, signs.x, delta->x) || !resize(frame.axisV, signs.y, delta->y)) return std::nullopt;
        auto anchor = (frame.target.boundsMin + frame.target.boundsMax) * 0.5f;
        if (signs.x != 0) anchor[frame.axisU] = signs.x > 0 ? frame.target.boundsMin[frame.axisU] : frame.target.boundsMax[frame.axisU];
        if (signs.y != 0) anchor[frame.axisV] = signs.y > 0 ? frame.target.boundsMin[frame.axisV] : frame.target.boundsMax[frame.axisV];
        auto compensation = Point(frame.world, anchor) - Point(World(frame.target, result), anchor);
        if (frame.view.is2D) compensation.z = 0.0f;
        result.localPosition += Direction(frame.inverseParent, compensation);
    }
    else
    {
        const auto angle = RotationAngle(frame, *point);
        if (!angle) return std::nullopt;
        drag.accumulatedAngle += std::remainder(*angle - drag.lastAngle, 2.0f * kPi);
        drag.lastAngle = *angle;
        const float radians = snap.enabled
            ? Snap(drag.accumulatedAngle, snap.rotationDegrees * kPi / 180.0f) : drag.accumulatedAngle;
        if (radians == 0.0f) return result;
        if (frame.view.is2D)
        {
            const auto delta = AxisAngle(Maths::Vector3::Forward, radians);
            result.localRotation = Maths::Quaternion::Normalize(Maths::Quaternion::Inverse(frame.target.parentRotation) *
                delta * frame.target.parentRotation * result.localRotation);
        }
        else result.localRotation = Maths::Quaternion::Normalize(result.localRotation *
            AxisAngle(Maths::Vector3::Cross(Axis(frame.axisU), Axis(frame.axisV)), radians));
        if (frame.pivot == SceneViewGizmoPivot::Center)
        {
            const auto center = (frame.target.boundsMin + frame.target.boundsMax) * 0.5f;
            auto compensation = frame.pivotWorld - Point(World(frame.target, result), center);
            if (frame.view.is2D) compensation.z = 0.0f;
            result.localPosition += Direction(frame.inverseParent, compensation);
        }
    }
    if (!Finite(result.localPosition) || !Finite(result.localScale) ||
        !std::isfinite(result.localRotation.x) || !std::isfinite(result.localRotation.y) ||
        !std::isfinite(result.localRotation.z) || !std::isfinite(result.localRotation.w)) return std::nullopt;
    return result;
}

void DrawRectToolOverlay(ImDrawList& drawList, const RectToolFrame& frame, const RectToolHandle hovered,
    const RectToolHandle active, const float uiScale)
{
    const ImU32 normal = IM_COL32(90, 175, 255, 255), hot = IM_COL32(255, 205, 80, 255);
    const auto position = [](const Maths::Vector2& p) { return ImVec2(p.x, p.y); };
    drawList.PushClipRect(position(frame.view.min), position(frame.view.max), true);
    for (int i = 0; i < 4; ++i)
    {
        const auto a = frame.corners[i], b = frame.corners[(i + 1) % 4];
        drawList.AddLine(position(a), position(b), hovered == kEdges[i] || active == kEdges[i] ? hot : normal, 1.5f * uiScale);
        for (int type = 0; type < 2; ++type)
        {
            const auto p = type == 0 ? a : (a + b) * 0.5f;
            const auto handle = type == 0 ? kCorners[i] : kEdges[i];
            const float radius = 3.0f * uiScale;
            drawList.AddRectFilled(ImVec2(p.x - radius, p.y - radius), ImVec2(p.x + radius, p.y + radius),
                handle == hovered || handle == active ? hot : normal);
        }
    }
    drawList.PopClipRect();
    const auto handle = active != RectToolHandle::None ? active : hovered;
    if (handle == RectToolHandle::None) return;
    if (GetRectToolHandleOperation(handle) == EGizmoOperation::ROTATE)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        auto* foreground = ImGui::GetForegroundDrawList();
        const auto mouse = ImGui::GetIO().MousePos;
        const ImVec2 center(mouse.x + 10.0f * uiScale, mouse.y + 10.0f * uiScale);
        foreground->PathArcTo(center, 7.0f * uiScale, -0.5f * kPi, kPi, 20);
        foreground->PathStroke(hot, 0, 2.0f * uiScale);
        foreground->AddTriangleFilled(ImVec2(center.x - 11 * uiScale, center.y - 2 * uiScale),
            ImVec2(center.x - 3 * uiScale, center.y - 2 * uiScale), ImVec2(center.x - 7 * uiScale, center.y + 5 * uiScale), hot);
    }
    else if (handle == RectToolHandle::Move) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    else
    {
        Maths::Vector2 center {}, handlePosition {};
        for (const auto& corner : frame.corners) center += corner * 0.25f;
        for (int i = 0; i < 4; ++i)
        {
            if (handle == kCorners[i]) handlePosition = frame.corners[i];
            if (handle == kEdges[i]) handlePosition = (frame.corners[i] + frame.corners[(i + 1) % 4]) * 0.5f;
        }
        const auto direction = handlePosition - center;
        const int sector = static_cast<int>(std::round(std::atan2(direction.y, direction.x) / (kPi * 0.25f)) + 8) % 4;
        constexpr std::array cursors {ImGuiMouseCursor_ResizeEW, ImGuiMouseCursor_ResizeNWSE,
            ImGuiMouseCursor_ResizeNS, ImGuiMouseCursor_ResizeNESW};
        ImGui::SetMouseCursor(cursors[sector]);
    }
}
}
