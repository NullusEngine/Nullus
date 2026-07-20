#pragma once

#include "Rendering/Assets/StaticMeshBuildDef.h"
#include "Rendering/Assets/StaticMeshBuilder.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace NLS::Render::Assets
{
struct StaticMeshCookRequest
{
    StaticMeshSourceAsset sourceAsset;
    MeshArtifactData importedLOD0;
    std::string sourceAssetIdentity;
    std::string sourceContentHash;
    std::string targetPlatform;
    std::string importerId = "static-mesh-cook";
    uint32_t importerVersion = 1u;
    uint32_t postprocessorVersion = 1u;
    std::string reducerId = "meshoptimizer";
    uint32_t reducerVersion = 1u;
    std::filesystem::path outputPath;
};

struct StaticMeshCookResult
{
    bool success = false;
    bool cacheHit = false;
    std::string buildIdentity;
    std::vector<uint8_t> artifactBytes;
    std::vector<std::string> diagnostics;
};

NLS_STATIC_MESH_BUILD_API std::string BuildStaticMeshCookIdentity(
    const StaticMeshCookRequest& request,
    const StaticMeshLODSettingsRegistry& settings = StaticMeshLODSettingsRegistry{});
NLS_STATIC_MESH_BUILD_API StaticMeshCookResult BuildStaticMeshCookArtifact(
    const StaticMeshCookRequest& request,
    const StaticMeshLODSettingsRegistry& settings = StaticMeshLODSettingsRegistry{});
}
