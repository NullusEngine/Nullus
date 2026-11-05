#pragma once

#include <cstdint>
#include <vector>
#include <algorithm>
#include <Math/Vector4.h>
#include <Rendering/Resources/Material.h>
#include <Rendering/Debug/SceneDisplayDebug.h>

namespace NLS::Engine::Rendering
{
enum class SceneGeometryDrawMode : uint8_t
{
    Shaded,
    Wireframe,
    ShadedWireframe
};

using SceneDisplayDebugMode = Render::Debug::SceneDisplayDebugMode;

inline std::string_view GetSceneDebugLightMode(const Render::Resources::Material& material)
{
    return material.ResolveShaderForLightMode("SceneDebug") ? std::string_view("SceneDebug") : std::string_view{};
}

struct SceneWireOverlayDescriptor { bool selected = false; };

// Per-view overrides, copied into the frame capture before render-thread work.
// Defaults preserve the normal game and asset-preview rendering paths.
struct SceneRenderOptions
{
    SceneGeometryDrawMode geometryMode = SceneGeometryDrawMode::Shaded;
    SceneDisplayDebugMode debugMode = SceneDisplayDebugMode::None;
    Render::Debug::SceneDebugParameters debugParameters;
    bool postProcessing = true;
    // Sorted owner ids, frozen with the view. Never modify the shared scene store.
    std::vector<int32_t> hiddenObjectIds;
    std::vector<int32_t> selectionWireIds;
    bool IsObjectHidden(int32_t id) const
    {
        return std::binary_search(hiddenObjectIds.begin(), hiddenObjectIds.end(), id);
    }
    bool previewLighting = false;
    bool includeParticles = true;
    Maths::Vector4 backgroundColor{0.0f, 0.0f, 0.0f, 0.0f};

    bool operator==(const SceneRenderOptions&) const = default;
};

inline void ApplySceneGeometryOverrides(const SceneRenderOptions& options,
    Render::Resources::MaterialPipelineStateOverrides& overrides)
{
    if (options.geometryMode == SceneGeometryDrawMode::Wireframe) overrides.wireframe = true;
}

inline Render::Resources::MaterialPipelineStateOverrides BuildSceneWireframeOverlayOverrides()
{
    Render::Resources::MaterialPipelineStateOverrides overrides;
    overrides.wireframe = true;
    overrides.depthTest = true;
    overrides.depthWrite = false;
    overrides.blending = false;
    overrides.depthCompare = Render::Settings::EComparaisonAlgorithm::LESS_EQUAL;
    return overrides;
}
}
