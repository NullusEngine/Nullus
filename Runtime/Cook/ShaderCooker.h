#pragma once

#include "CookDef.h"
#include "CookTypes.h"

#include "Assets/ArtifactManifest.h"
#include "Assets/ArtifactWriter.h"

#include <filesystem>
#include <string>
#include <vector>

namespace NLS::Cook::Detail
{
struct ShaderCookOutput
{
    std::vector<NLS::Core::Assets::ArtifactPayload> payloads;
    std::vector<NLS::Core::Assets::AssetDependencyRecord> dependencies;
    std::vector<CookDiagnostic> diagnostics;
    std::string primarySubAssetKey;
};

// Cooks a ShaderLab asset into one or more native shader artifacts. Every
// LightMode pass becomes a sub-asset; the Forward pass (or the first pass when
// no Forward pass exists) is the primary artifact.
NLS_COOK_API bool CookShaderAsset(
    const CookOptions& options,
    const std::filesystem::path& source,
    const NLS::Core::Assets::AssetMeta& meta,
    ShaderCookOutput& output,
    std::string& error);
} // namespace NLS::Cook::Detail
