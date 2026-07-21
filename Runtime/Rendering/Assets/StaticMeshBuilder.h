#pragma once

#include "Rendering/Assets/StaticMeshBuildDef.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/StaticMeshLODSettings.h"

#include <string>
#include <vector>

namespace NLS::Render::Assets
{
struct StaticMeshLODBuildResult
{
    bool success = false;
    MeshArtifactBundle bundle;
    std::vector<std::string> diagnostics;
};

NLS_STATIC_MESH_BUILD_API StaticMeshLODBuildResult BuildStaticMeshLODArtifact(
    const StaticMeshSourceAsset& sourceAsset,
    const MeshArtifactData& importedLOD0,
    const StaticMeshLODSettingsRegistry& settings = StaticMeshLODSettingsRegistry{});
}
