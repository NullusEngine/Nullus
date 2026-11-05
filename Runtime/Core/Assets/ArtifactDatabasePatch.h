#pragma once

#include "Assets/ArtifactDatabase.h"

namespace NLS::Core::Assets
{
// Source-scoped LMDB operations. Other sources and target platforms are read
// and modified by their own transactions, never replaced by an old snapshot.
NLS_CORE_API std::optional<ArtifactManifest> ReadArtifactDatabaseManifest(
    const std::filesystem::path& path, AssetId sourceAssetId, const std::string& targetPlatform,
    std::string* error = nullptr);
NLS_CORE_API bool WriteArtifactDatabaseManifest(
    const std::filesystem::path& path, const ArtifactManifest& manifest,
    const std::string& sourcePath, ArtifactRecordStatus status,
    const ArtifactManifest* expectedPrevious = nullptr, std::string* error = nullptr);
}
