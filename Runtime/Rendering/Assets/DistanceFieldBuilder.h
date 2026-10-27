#pragma once

#include <atomic>
#include <span>
#include <map>
#include "Assets/ArtifactWriter.h"
#include "Rendering/Assets/StaticMeshBuildDef.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/ImportedScene.h"
#include "Rendering/DistanceField/DistanceFieldTypes.h"

namespace NLS::Render::DistanceField
{
    struct DistanceFieldBuildResult
    {
        bool success = false;
        std::shared_ptr<MeshDistanceFieldArtifact> asset;
        std::vector<std::string> diagnostics;
    };

    NLS_STATIC_MESH_BUILD_API DistanceFieldBuildResult BuildMeshDistanceField(
        std::span<const Assets::MeshArtifactData> sections, const DistanceFieldBuildSettings& settings,
        const std::atomic_bool* cancellation = nullptr,
        const NLS::Core::Assets::IArtifactWriteCancellation* importCancellation = nullptr);

    NLS_STATIC_MESH_BUILD_API DistanceFieldBuildSettings ReadDistanceFieldBuildSettings(
        const std::map<std::string, std::string>& settings, const std::filesystem::path& projectRoot = {});
    NLS_STATIC_MESH_BUILD_API bool AppendDistanceFieldPayloads(const std::string& meshKey,
        std::span<const Assets::MeshArtifactData> sections, const DistanceFieldBuildSettings& settings,
        std::vector<NLS::Core::Assets::ArtifactPayload>& payloads, std::vector<std::string>& diagnostics,
        const std::atomic_bool* cancellation = nullptr,
        const NLS::Core::Assets::IArtifactWriteCancellation* importCancellation = nullptr);
    NLS_STATIC_MESH_BUILD_API bool AppendDistanceFieldModelPayloads(const Assets::ImportedScene& scene,
        const std::map<std::string,Assets::MeshArtifactData>& meshes, const DistanceFieldBuildSettings& settings,
        std::vector<NLS::Core::Assets::ArtifactPayload>& payloads, std::vector<std::string>& diagnostics,
        const std::atomic_bool* cancellation = nullptr,
        const NLS::Core::Assets::IArtifactWriteCancellation* importCancellation = nullptr);
}
