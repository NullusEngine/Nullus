#pragma once

#include <array>
#include <optional>

#include "Core/SceneViewImGuizmo.h"

struct ImDrawList;

namespace NLS::Editor::Core
{
enum class RectToolPlane { XY, XZ, YZ };
enum class RectToolHandle
{
    None, Move,
    BottomLeft, BottomRight, TopRight, TopLeft,
    Bottom, Right, Top, Left,
    RotateBottomLeft, RotateBottomRight, RotateTopRight, RotateTopLeft
};

struct RectToolTransform
{
    Maths::Vector3 localPosition {};
    Maths::Quaternion localRotation {};
    Maths::Vector3 localScale {1.0f, 1.0f, 1.0f};
};

bool operator==(const RectToolTransform& left, const RectToolTransform& right);

struct RectToolTarget
{
    RectToolTransform transform;
    Maths::Matrix4 parentWorld = Maths::Matrix4::Identity;
    Maths::Quaternion parentRotation {};
    Maths::Vector3 boundsMin {-0.5f, -0.5f, -0.5f};
    Maths::Vector3 boundsMax {0.5f, 0.5f, 0.5f};
};

struct RectToolView
{
    Maths::Matrix4 viewProjection = Maths::Matrix4::Identity;
    Maths::Vector3 cameraPosition {};
    Maths::Vector3 cameraForward = -Maths::Vector3::Forward;
    Maths::Vector2 min {};
    Maths::Vector2 max {};
    bool orthographic = false;
    bool is2D = false;
};

struct RectToolFrame
{
    RectToolTarget target;
    RectToolView view;
    RectToolPlane plane = RectToolPlane::XY;
    SceneViewGizmoPivot pivot = SceneViewGizmoPivot::Pivot;
    int axisU = 0;
    int axisV = 1;
    Maths::Matrix4 world = Maths::Matrix4::Identity;
    Maths::Matrix4 inverseParent = Maths::Matrix4::Identity;
    Maths::Matrix4 inverseViewProjection = Maths::Matrix4::Identity;
    std::array<Maths::Vector3, 4> localCorners {};
    std::array<Maths::Vector2, 4> corners {};
    Maths::Vector3 origin {};
    Maths::Vector3 normal {};
    Maths::Vector3 basisU {};
    Maths::Vector3 basisV {};
    Maths::Vector3 rotationBasisU {};
    Maths::Vector3 rotationBasisV {};
    Maths::Vector3 pivotWorld {};
};

struct RectToolSnap
{
    bool enabled = false;
    float translation = 1.0f;
    float rotationDegrees = 15.0f;
    float scale = 0.1f;
};

struct RectToolDrag
{
    RectToolFrame frame;
    RectToolHandle handle = RectToolHandle::None;
    Maths::Vector3 startPoint {};
    float lastAngle = 0.0f;
    float accumulatedAngle = 0.0f;
};

RectToolTarget CaptureRectToolTarget(const Engine::GameObject& object);
std::optional<RectToolFrame> BuildRectToolFrame(const RectToolTarget& target, const RectToolView& view,
    SceneViewGizmoPivot pivot, std::optional<RectToolPlane> lockedPlane = std::nullopt);
RectToolHandle HitTestRectTool(const RectToolFrame& frame, const Maths::Vector2& mouse, float uiScale = 1.0f);
EGizmoOperation GetRectToolHandleOperation(RectToolHandle handle);
std::optional<RectToolDrag> BeginRectToolDrag(const RectToolFrame& frame, RectToolHandle handle,
    const Maths::Vector2& mouse);
std::optional<RectToolTransform> UpdateRectToolDrag(RectToolDrag& drag, const Maths::Vector2& mouse,
    const RectToolSnap& snap);
void DrawRectToolOverlay(ImDrawList& drawList, const RectToolFrame& frame, RectToolHandle hovered,
    RectToolHandle active, float uiScale);
}
