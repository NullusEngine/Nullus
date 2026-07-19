#pragma once

namespace NLS::Editor::Assets::ThumbnailPreviewCamera
{
inline constexpr float FieldOfViewDegrees = 30.0f;
inline constexpr float MeshYawDegrees = -120.0f;
// Nullus is Y-up; a negative look pitch places the camera above the asset.
inline constexpr float MeshLookPitchDegrees = -20.0f;
inline constexpr float PrefabYawDegrees = -135.0f;
inline constexpr float PrefabLookPitchDegrees = -36.0f;
inline constexpr float MaterialDistance = 5.0f;
inline constexpr float DegreesToRadians = 3.14159265358979323846f / 180.0f;
}
