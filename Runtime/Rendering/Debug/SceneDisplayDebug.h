#pragma once

#include <cstdint>
#include <Math/Vector4.h>

namespace NLS::Render::Debug
{
// Values are shared with SceneDebug.hlsl and the particle view constants.
enum class SceneDisplayDebugMode : uint8_t
{
    None, Albedo, Specular, Smoothness, Normal, RenderPaths, AlphaChannel,
    Overdraw, Mipmaps, ValidateAlbedo, ValidateMetalSpecular
};

struct SceneDebugParameters
{
    float overdrawMax = 16.0f;
    // Dielectric min/max, metal min/max, in 8-bit sRGB units.
    Maths::Vector4 albedoRange{50.0f, 243.0f, 186.0f, 255.0f};
    Maths::Vector4 specularRange{40.0f, 75.0f, 155.0f, 255.0f};
    bool operator==(const SceneDebugParameters&) const = default;
};

inline bool IsGBufferDebugMode(SceneDisplayDebugMode mode)
{
    return mode >= SceneDisplayDebugMode::Albedo && mode <= SceneDisplayDebugMode::Normal;
}
}
