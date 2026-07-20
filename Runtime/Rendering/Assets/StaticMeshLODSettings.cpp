#include "Rendering/Assets/StaticMeshLODSettings.h"

#include <algorithm>

namespace NLS::Render::Assets
{
StaticMeshLODSettingsRegistry::StaticMeshLODSettingsRegistry()
    : m_presets {
        {"None", 1u, 100.0f, 0.0f},
        {"LevelArchitecture", 4u, 50.0f, 12.0f},
        {"SmallProp", 4u, 50.0f, 10.0f},
        {"LargeProp", 4u, 50.0f, 10.0f},
        {"Deco", 4u, 50.0f, 10.0f},
        {"Vista", 1u, 100.0f, 0.0f},
        {"Foliage", 1u, 100.0f, 0.0f},
        {"HighDetail", 6u, 50.0f, 6.0f}}
{
}

const StaticMeshLODGroupPreset* StaticMeshLODSettingsRegistry::Find(
    const std::string_view name) const
{
    const auto it = std::find_if(
        m_presets.begin(),
        m_presets.end(),
        [name](const StaticMeshLODGroupPreset& preset)
        {
            return preset.name == name;
        });
    return it == m_presets.end() ? nullptr : &*it;
}

const std::vector<StaticMeshLODGroupPreset>& StaticMeshLODSettingsRegistry::GetPresets() const
{
    return m_presets;
}

std::vector<float> BuildStaticMeshLODTargetRatios(const StaticMeshLODGroupPreset& preset)
{
    std::vector<float> ratios;
    ratios.reserve(preset.numLODs);
    if (preset.numLODs == 0u)
        return ratios;

    const auto reductionRatio = preset.lodPercentTriangles * 0.01f;
    auto ratio = 1.0f;
    for (uint32_t lodIndex = 0u; lodIndex < preset.numLODs; ++lodIndex)
    {
        ratios.push_back(ratio);
        ratio *= reductionRatio;
    }
    return ratios;
}

StaticMeshLODValidationResult ValidateStaticMeshSourceAsset(
    const StaticMeshSourceAsset& asset)
{
    StaticMeshLODValidationResult result;
    if (asset.sourceModels.empty())
    {
        result.diagnostics.push_back("static-mesh-lod0-missing");
        return result;
    }

    float previousScreenSize = asset.sourceModels.front().screenSize;
    if (!(previousScreenSize > 0.0f))
    {
        result.diagnostics.push_back("static-mesh-lod-screen-size-invalid");
        return result;
    }
    for (size_t index = 1u; index < asset.sourceModels.size(); ++index)
    {
        const float screenSize = asset.sourceModels[index].screenSize;
        if (!(screenSize > 0.0f) || !(screenSize < previousScreenSize))
        {
            result.diagnostics.push_back("static-mesh-lod-screen-size-not-descending");
            return result;
        }
        previousScreenSize = screenSize;
    }

    result.valid = true;
    return result;
}
}
