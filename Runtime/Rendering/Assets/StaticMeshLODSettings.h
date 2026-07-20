#pragma once

#include "RenderDef.h"
#include "Rendering/Assets/MeshArtifact.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace NLS::Render::Assets
{
enum class StaticMeshLODSourceKind
{
    Imported,
    Authored,
    Generated
};

struct StaticMeshSourceModel
{
    StaticMeshLODSourceKind sourceKind = StaticMeshLODSourceKind::Imported;
    float screenSize = 1.0f;
    MeshArtifactData mesh;
};

struct StaticMeshSourceAsset
{
    std::string lodGroup = "None";
    uint32_t minLOD = 0u;
    bool autoComputeLODScreenSize = true;
    std::vector<StaticMeshSourceModel> sourceModels;
};

struct StaticMeshLODValidationResult
{
    bool valid = false;
    std::vector<std::string> diagnostics;
};

struct StaticMeshLODGroupPreset
{
    std::string name;
    uint32_t numLODs = 1u;
    float lodPercentTriangles = 100.0f;
    float pixelError = 0.0f;
};

class NLS_RENDER_API StaticMeshLODSettingsRegistry
{
public:
    StaticMeshLODSettingsRegistry();

    [[nodiscard]] const StaticMeshLODGroupPreset* Find(std::string_view name) const;
    [[nodiscard]] const std::vector<StaticMeshLODGroupPreset>& GetPresets() const;

private:
    std::vector<StaticMeshLODGroupPreset> m_presets;
};

NLS_RENDER_API std::vector<float> BuildStaticMeshLODTargetRatios(
    const StaticMeshLODGroupPreset& preset);
NLS_RENDER_API StaticMeshLODValidationResult ValidateStaticMeshSourceAsset(
    const StaticMeshSourceAsset& asset);
}
