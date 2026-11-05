#pragma once

#include "CookDef.h"
#include "CookTypes.h"

#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/ArtifactWriter.h"

#include <filesystem>
#include <string>
#include <vector>

namespace NLS::Cook::Detail
{
struct ModelCookOutput
{
    std::vector<NLS::Core::Assets::ArtifactPayload> payloads;
    std::vector<NLS::Core::Assets::AssetDependencyRecord> dependencies;
    std::vector<CookDiagnostic> diagnostics;
    std::string primarySubAssetKey;
    bool incremental = false;
    std::vector<std::string> removedSubAssets;
};

// Cooks a model scene (glTF/GLB/FBX/OBJ) into mesh, material, and prefab
// artifacts. External textures must already be cooked; embedded textures are
// reported as unsupported in this phase.
NLS_COOK_API bool CookModelAsset(
    const CookOptions& options,
    const std::filesystem::path& source,
    const NLS::Core::Assets::AssetMeta& meta,
    NLS::Core::Assets::ArtifactDatabase& database,
    ModelCookOutput& output,
    std::string& error);
NLS_COOK_API bool CookModelDistanceFields(
    const CookOptions& options, const NLS::Core::Assets::AssetMeta& meta,
    const NLS::Core::Assets::ArtifactManifest& previous, ModelCookOutput& output,
    std::string& error);
} // namespace NLS::Cook::Detail
