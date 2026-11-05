#pragma once

#include <cstddef>
#include <span>
#include "Core/SceneViewImGuizmo.h"
#include "Core/SceneCameraFocus.h"
#include <Rendering/Entities/Camera.h>
#include <Engine/Rendering/SceneRenderOptions.h>

namespace NLS::Editor::Core
{
enum class SceneViewDrawMode
{
    Shaded,
    Wireframe,
    ShadedWireframe,
    Albedo,
    Specular,
    Smoothness,
    Normal,
    RenderPaths,
    AlphaChannel,
    Overdraw,
    Mipmaps,
    ValidateAlbedo,
    ValidateMetalSpecular,
    MeshDistanceField,
    GlobalDistanceField
};

struct SceneViewDrawModeInfo
{
    SceneViewDrawMode mode;
    const char* label;
    const char* category;
    const char* legend;
    int persistedValue; // -1 for session-only modes
};

std::span<const SceneViewDrawModeInfo> GetSceneViewDrawModes();
const SceneViewDrawModeInfo& GetSceneViewDrawModeInfo(SceneViewDrawMode mode);
SceneViewDrawMode DecodeSceneViewDrawMode(int value);
void ApplySceneViewDrawMode(SceneViewDrawMode mode, Engine::Rendering::SceneRenderOptions& options);

struct SceneViewToolbarState
{
    EGizmoOperation operation = EGizmoOperation::TRANSLATE;
    SceneViewGizmoPivot pivot = SceneViewGizmoPivot::Pivot;
    SceneViewGizmoSpace space = SceneViewGizmoSpace::Global;
    SceneViewDrawMode drawMode = SceneViewDrawMode::Shaded;
    bool is2D = false;
};

struct SceneViewCameraSnapshot
{
    Maths::Vector3 position;
    Maths::Quaternion rotation;
    Render::Settings::EProjectionMode projection;
    float orthographicSize;
    SceneCameraFocusState focus;
};

struct SceneViewPlacementRay
{
    Maths::Vector3 origin;
    Maths::Vector3 direction;
};

SceneViewCameraSnapshot CaptureSceneViewCamera(const Render::Entities::Camera& camera,
    const SceneCameraFocusState& focus);
void RestoreSceneViewCamera(Render::Entities::Camera& camera, SceneCameraFocusState& focus,
    const SceneViewCameraSnapshot& snapshot);
void EnterSceneView2D(Render::Entities::Camera& camera, SceneCameraFocusState& focus);
SceneViewPlacementRay BuildSceneViewPlacementRay(const Render::Entities::Camera& camera,
    const Maths::Vector2& localPosition, const Maths::Vector2& viewportSize);
std::size_t GetVisibleSceneToolbarGroupCount(std::span<const float> widths,
    float availableWidth, float moreWidth, float spacing);
}
