#pragma once

#include "Assets/ArtifactDatabase.h"

#include <filesystem>
#include <optional>

namespace NLS::Editor::Assets
{
inline std::filesystem::path GetProjectArtifactDatabasePath(const std::filesystem::path& projectRoot)
{
    return projectRoot / "Library" / "ArtifactDB";
}

inline std::optional<NLS::Core::Assets::ArtifactManifest> LoadArtifactManifestFromProjectArtifactDB(
    const std::filesystem::path& projectRoot,
    const NLS::Core::Assets::AssetId sourceAssetId)
{
    if (!sourceAssetId.IsValid())
        return std::nullopt;

    NLS::Core::Assets::ArtifactDatabase database;
    if (!database.Load(GetProjectArtifactDatabasePath(projectRoot)))
        return std::nullopt;
    return database.BuildManifestForSource(sourceAssetId);
}
}
