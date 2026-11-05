#pragma once

#include <algorithm>
#include <cmath>
#include <Rendering/Entities/Camera.h>

namespace NLS::Editor::Rendering
{
struct SceneIconPresentation { float scale = 1.0f; float opacity = 1.0f; };

inline SceneIconPresentation ComputeSceneIconPresentation(const Render::Entities::Camera& camera,
    const Maths::Vector3& position, float viewportHeight, float meshHeight, float iconSize,
    bool icons3D, bool fade, float baseScale = 0.1f)
{
    const float depth = std::max(0.001f, std::abs(Maths::Vector3::Dot(position - camera.GetPosition(),
        camera.GetRotation() * Maths::Vector3::Forward)));
    const float halfHeight = camera.GetProjectionMode() == Render::Settings::EProjectionMode::ORTHOGRAPHIC ?
        camera.GetSize() : depth * std::tan(Maths::DegreesToRadians(camera.GetFov()) * 0.5f);
    const float unitsPerPixel = std::max(0.000001f, 2.0f * halfHeight / std::max(1.0f, viewportHeight));
    SceneIconPresentation result;
    result.scale = icons3D ? baseScale * iconSize :
        32.0f * (iconSize / 0.5f) * unitsPerPixel / std::max(0.001f, meshHeight);
    const float pixels = std::max(0.001f, meshHeight) * result.scale / unitsPerPixel;
    if (fade && icons3D) result.opacity = std::clamp((pixels - 2.0f) / 10.0f, 0.0f, 1.0f);
    return result;
}

inline float ComputeLightBillboardShaderScale(const Render::Entities::Camera& camera,
    const Maths::Vector3& position, const SceneIconPresentation& icon, bool icons3D)
{
    // The billboard shader multiplies u_Scale by the distance to the camera.
    return icons3D ? icon.scale : icon.scale / std::max(0.001f, Maths::Vector3::Distance(position, camera.GetPosition()));
}
}
