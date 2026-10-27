#pragma once

#include "BuildDef.h"
#include "BuildTypes.h"

#include <filesystem>
#include <string>
#include <vector>

#include "Engine/Assets/RuntimeAssetDatabase.h"

namespace NLS::Build
{
struct BuildReport;

struct CollectedArtifact
{
    std::string reference;
    std::string sourcePath;
    std::string artifactPath;
    std::filesystem::path hostPath;
    uint64_t size = 0u;
    std::string reason;
    std::string package;
};

struct CollectedRawFile
{
    std::string relativePath;
    std::filesystem::path hostPath;
    uint64_t size = 0u;
    std::string reason;
};

struct DependencyCollectionResult
{
    NLS::Engine::Assets::RuntimeAssetManifest manifest;
    std::vector<CollectedArtifact> artifacts;
    std::vector<CollectedRawFile> rawFiles;
    // Per-package artifact path lists; the base package always exists.
    std::vector<std::pair<std::string, std::vector<std::string>>> packageArtifactPaths;
};

// Collects the dependency closure for the profile's release scenes and keep
// rules. Reports every included asset with the reason it was reached, rejects
// missing references, duplicate GUIDs, and unsupported asset types.
class NLS_BUILD_API BuildDependencyCollector
{
public:
    static bool Collect(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& projectAssetsPath,
        const BuildProfile& profile,
        const std::filesystem::path& artifactDatabasePath,
        DependencyCollectionResult& result,
        BuildReport& report);

    // Maps an artifact storage path ("Library/Artifacts/ab/<hash>") onto the
    // host file the project stores.
    static std::filesystem::path ResolveArtifactHostPath(
        const std::filesystem::path& projectRoot,
        const std::string& artifactPath);

    // Loads the AssetId a project-relative source path maps to via its .meta
    // sidecar. Returns nullopt when the meta file is missing or invalid.
    static std::optional<NLS::Core::Assets::AssetId> LoadAssetIdForPath(
        const std::filesystem::path& projectAssetsPath,
        const std::string& projectRelativePath,
        std::string* error);

    static bool MatchesWildcard(const std::string& pattern, const std::string& value);
};
} // namespace NLS::Build
