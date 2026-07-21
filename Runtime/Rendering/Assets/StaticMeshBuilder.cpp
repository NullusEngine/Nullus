#include "Rendering/Assets/StaticMeshBuilder.h"

#include "Rendering/Assets/MeshReduction.h"

#include <algorithm>
#include <cmath>

namespace NLS::Render::Assets
{
namespace
{
bool HasMeshData(const MeshArtifactData& mesh)
{
    return !mesh.vertices.empty() && !mesh.indices.empty();
}

uint32_t FindRequiredLODCount(
    const StaticMeshSourceAsset& sourceAsset,
    const StaticMeshLODGroupPreset& preset)
{
    uint32_t requiredCount = preset.numLODs;
    for (size_t index = 1u; index < sourceAsset.sourceModels.size(); ++index)
    {
        if (sourceAsset.sourceModels[index].sourceKind == StaticMeshLODSourceKind::Authored)
        {
            requiredCount = (std::max)(
                requiredCount,
                static_cast<uint32_t>(index + 1u));
        }
    }
    return (std::max)(requiredCount, 1u);
}

float ComputeGeneratedScreenSize(const uint32_t lodIndex)
{
    return std::ldexp(1.0f, -static_cast<int>(lodIndex));
}
}

StaticMeshLODBuildResult BuildStaticMeshLODArtifact(
    const StaticMeshSourceAsset& sourceAsset,
    const MeshArtifactData& importedLOD0,
    const StaticMeshLODSettingsRegistry& settings)
{
    StaticMeshLODBuildResult result;
    const auto* preset = settings.Find(sourceAsset.lodGroup);
    if (preset == nullptr)
    {
        result.diagnostics.push_back("static-mesh-lod-group-unknown");
        return result;
    }

    const auto lod0TriangleCount = importedLOD0.indices.size() / 3u;
    auto validatedLOD0 = ReduceMeshArtifact(
        importedLOD0,
        static_cast<uint32_t>(lod0TriangleCount));
    if (!validatedLOD0.has_value())
    {
        result.diagnostics.push_back("static-mesh-lod0-invalid");
        return result;
    }

    if (!sourceAsset.sourceModels.empty())
    {
        const auto validation = ValidateStaticMeshSourceAsset(sourceAsset);
        if (!validation.valid)
        {
            result.diagnostics = validation.diagnostics;
            return result;
        }
    }

    const auto requiredLODCount = FindRequiredLODCount(sourceAsset, *preset);
    const auto ratios = BuildStaticMeshLODTargetRatios({
        preset->name,
        requiredLODCount,
        preset->lodPercentTriangles,
        preset->pixelError});

    result.bundle.lodResources.reserve(requiredLODCount);
    result.bundle.lodResources.push_back({std::move(*validatedLOD0), 1.0f});
    for (uint32_t lodIndex = 1u; lodIndex < requiredLODCount; ++lodIndex)
    {
        const StaticMeshSourceModel* sourceModel =
            lodIndex < sourceAsset.sourceModels.size()
            ? &sourceAsset.sourceModels[lodIndex]
            : nullptr;
        if (sourceModel != nullptr &&
            sourceModel->sourceKind == StaticMeshLODSourceKind::Authored)
        {
            if (!HasMeshData(sourceModel->mesh))
            {
                result.bundle.lodResources.clear();
                result.diagnostics.push_back("static-mesh-authored-lod-missing-data");
                return result;
            }
            const auto authoredTriangleCount = sourceModel->mesh.indices.size() / 3u;
            auto authored = ReduceMeshArtifact(
                sourceModel->mesh,
                static_cast<uint32_t>(authoredTriangleCount));
            if (!authored.has_value())
            {
                result.bundle.lodResources.clear();
                result.diagnostics.push_back("static-mesh-authored-lod-invalid");
                return result;
            }
            result.bundle.lodResources.push_back({
                std::move(*authored),
                sourceModel->screenSize});
            continue;
        }

        const auto targetTriangleCount = (std::max)(
            1u,
            static_cast<uint32_t>(std::floor(
                static_cast<double>(lod0TriangleCount) * ratios[lodIndex])));
        auto generated = ReduceMeshArtifact(importedLOD0, targetTriangleCount);
        if (!generated.has_value())
        {
            result.bundle.lodResources.clear();
            result.diagnostics.push_back("static-mesh-generated-lod-failed");
            return result;
        }
        const float screenSize =
            sourceModel != nullptr && !sourceAsset.autoComputeLODScreenSize
            ? sourceModel->screenSize
            : ComputeGeneratedScreenSize(lodIndex);
        result.bundle.lodResources.push_back({std::move(*generated), screenSize});
    }

    if (sourceAsset.minLOD >= result.bundle.lodResources.size())
    {
        result.bundle.lodResources.clear();
        result.diagnostics.push_back("static-mesh-min-lod-out-of-range");
        return result;
    }

    result.bundle.minLOD = sourceAsset.minLOD;

    result.success = true;
    return result;
}
}
