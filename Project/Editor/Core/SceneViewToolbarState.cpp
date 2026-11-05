#include "Core/SceneViewToolbarState.h"

#include <algorithm>
#include <cmath>
#include <Math/Transform.h>

namespace NLS::Editor::Core
{
std::span<const SceneViewDrawModeInfo> GetSceneViewDrawModes()
{
    using M = SceneViewDrawMode;
    static constexpr SceneViewDrawModeInfo modes[]{
        {M::Shaded, "Shaded", "Shading Mode", "Normal material shading.", 0},
        {M::Wireframe, "Wireframe", "Shading Mode", "Mesh edges.", 1},
        {M::ShadedWireframe, "Shaded Wireframe", "Shading Mode", "Shaded surfaces with depth-tested edges.", 2},
        {M::RenderPaths, "Render Paths", "Miscellaneous", "Blue: deferred. Yellow: forward, transparent and VFX.", 7},
        {M::AlphaChannel, "Alpha Channel", "Miscellaneous", "Black: empty. White: opaque. Gray: blended coverage. Unsupported custom shaders are neutral gray. Sky and editor helpers are excluded.", 8},
        {M::Overdraw, "Overdraw", "Miscellaneous", "Fragment coverage layers, including occluded geometry. Blue to red: increasing layers. Gray: unsupported custom shader.", 9},
        {M::Mipmaps, "Mipmaps", "Miscellaneous", "Red: oversized texture. Blue: insufficient resolution. Green: within one mip. Gray: no main texture.", 10},
        {M::Albedo, "Albedo", "Deferred", "GBuffer base color. Gray: forward surface or VFX, without GBuffer data.", 3},
        {M::Specular, "Specular", "Deferred", "GBuffer PBR F0. Gray: forward surface or VFX.", 4},
        {M::Smoothness, "Smoothness", "Deferred", "One minus GBuffer roughness. Gray: forward surface or VFX.", 5},
        {M::Normal, "Normal", "Deferred", "World-space shading normal, encoded as RGB. Gray: forward surface or VFX.", 6},
        {M::ValidateAlbedo, "Validate Albedo", "Material Validator", "Red: below range. Blue: above range. Gray: no PBR surface data.", 11},
        {M::ValidateMetalSpecular, "Validate Metal Specular", "Material Validator", "Validate PBR F0. Red: below range. Blue: above range. Gray: no PBR surface data.", 12},
        {M::MeshDistanceField, "Mesh Distance Field", "Distance Fields", "Mesh signed distance fields.", -1},
        {M::GlobalDistanceField, "Global Distance Field", "Distance Fields", "Global signed distance field.", -1}};
    return modes;
}

const SceneViewDrawModeInfo& GetSceneViewDrawModeInfo(SceneViewDrawMode mode)
{
    const auto modes = GetSceneViewDrawModes();
    for (const auto& entry : modes) if (entry.mode == mode) return entry;
    return modes.front();
}

SceneViewDrawMode DecodeSceneViewDrawMode(int value)
{
    for (const auto& entry : GetSceneViewDrawModes())
        if (entry.persistedValue >= 0 && entry.persistedValue == value) return entry.mode;
    return SceneViewDrawMode::Shaded;
}

void ApplySceneViewDrawMode(SceneViewDrawMode mode, Engine::Rendering::SceneRenderOptions& options)
{
    using G = Engine::Rendering::SceneGeometryDrawMode;
    using D = Engine::Rendering::SceneDisplayDebugMode;
    options.geometryMode = G::Shaded;
    options.debugMode = D::None;
    switch (mode)
    {
    case SceneViewDrawMode::Wireframe: options.geometryMode = G::Wireframe; break;
    case SceneViewDrawMode::ShadedWireframe: options.geometryMode = G::ShadedWireframe; break;
    case SceneViewDrawMode::Albedo: options.debugMode = D::Albedo; break;
    case SceneViewDrawMode::Specular: options.debugMode = D::Specular; break;
    case SceneViewDrawMode::Smoothness: options.debugMode = D::Smoothness; break;
    case SceneViewDrawMode::Normal: options.debugMode = D::Normal; break;
    case SceneViewDrawMode::RenderPaths: options.debugMode = D::RenderPaths; break;
    case SceneViewDrawMode::AlphaChannel: options.debugMode = D::AlphaChannel; break;
    case SceneViewDrawMode::Overdraw: options.debugMode = D::Overdraw; break;
    case SceneViewDrawMode::Mipmaps: options.debugMode = D::Mipmaps; break;
    case SceneViewDrawMode::ValidateAlbedo: options.debugMode = D::ValidateAlbedo; break;
    case SceneViewDrawMode::ValidateMetalSpecular: options.debugMode = D::ValidateMetalSpecular; break;
    default: break;
    }
    if (options.debugMode != D::None) options.postProcessing = false;
}

SceneViewCameraSnapshot CaptureSceneViewCamera(const Render::Entities::Camera& camera,
    const SceneCameraFocusState& focus)
{
    return {camera.GetPosition(), camera.GetRotation(), camera.GetProjectionMode(), camera.GetSize(), focus};
}

void RestoreSceneViewCamera(Render::Entities::Camera& camera, SceneCameraFocusState& focus,
    const SceneViewCameraSnapshot& snapshot)
{
    camera.SetPosition(snapshot.position);
    camera.SetRotation(snapshot.rotation);
    camera.SetProjectionMode(snapshot.projection);
    camera.SetSize(snapshot.orthographicSize);
    focus = snapshot.focus;
    camera.CacheViewMatrix();
}

void EnterSceneView2D(Render::Entities::Camera& camera, SceneCameraFocusState& focus)
{
    const float distance = std::max(1.0f, focus.hasFocus ? focus.focusDistance : 15.0f);
    const auto center = focus.hasFocus ? focus.focusPoint :
        camera.GetPosition() + camera.GetRotation() * Maths::Vector3::Forward * distance;
    if (camera.GetProjectionMode() == Render::Settings::EProjectionMode::PERSPECTIVE)
        camera.SetSize(std::max(0.01f, distance * std::tan(Maths::DegreesToRadians(camera.GetFov()) * 0.5f)));
    camera.SetProjectionMode(Render::Settings::EProjectionMode::ORTHOGRAPHIC);
    camera.SetRotation(Maths::Quaternion::LookAt(-Maths::Vector3::Forward, Maths::Vector3::Up));
    camera.SetPosition(center + Maths::Vector3::Forward * distance);
    focus = {center, distance, true};
    camera.CacheViewMatrix();
}

SceneViewPlacementRay BuildSceneViewPlacementRay(const Render::Entities::Camera& camera,
    const Maths::Vector2& localPosition, const Maths::Vector2& viewportSize)
{
    const float width = std::max(1.0f, viewportSize.x);
    const float height = std::max(1.0f, viewportSize.y);
    const float x = -(localPosition.x / width * 2.0f - 1.0f);
    const float y = 1.0f - localPosition.y / height * 2.0f;
    const auto rotation = camera.GetRotation();
    const auto forward = rotation * Maths::Vector3::Forward;
    const auto right = rotation * Maths::Vector3::Right;
    const auto up = rotation * Maths::Vector3::Up;
    if (camera.GetProjectionMode() == Render::Settings::EProjectionMode::ORTHOGRAPHIC)
        return {camera.GetPosition() + right * (x * camera.GetSize() * width / height) +
            up * (y * camera.GetSize()), forward};
    const float tanHalfFov = std::tan(Maths::DegreesToRadians(camera.GetFov()) * 0.5f);
    auto direction = forward + right * (x * tanHalfFov * width / height) + up * (y * tanHalfFov);
    direction.Normalise();
    return {camera.GetPosition(), direction};
}

std::size_t GetVisibleSceneToolbarGroupCount(const std::span<const float> widths,
    const float availableWidth, const float moreWidth, const float spacing)
{
    float used = 0.0f;
    for (std::size_t i = 0; i < widths.size(); ++i)
    {
        const float next = used + (i ? spacing : 0.0f) + widths[i];
        const float reserved = i + 1 < widths.size() ? spacing + moreWidth : 0.0f;
        if (next + reserved > availableWidth)
            return i;
        used = next;
    }
    return widths.size();
}
}
