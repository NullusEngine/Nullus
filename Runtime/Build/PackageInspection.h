#pragma once

#include "BuildDef.h"
#include "BuildTypes.h"

#include <filesystem>
#include <string>
#include <vector>

namespace NLS::Build
{
struct PackageValidationIssue
{
    std::string code;
    std::string message;
    std::string path;
};

struct PackageInspection
{
    std::filesystem::path packageDirectory;
    bool hasPlayerDescriptor = false;
    bool hasRuntimeManifest = false;
    std::string playerId;
    std::string targetId;
    std::string artifactPlatform;
    std::string engineVersion;
    std::string buildMode;
    std::string productName;
    std::string productVersion;
    std::string startScene;
    std::vector<std::string> contentPackages;
    std::vector<std::string> scriptBackends;
    uint64_t totalContentBytes = 0u;
    uint64_t rawContentBytes = 0u;
    uint64_t fileCount = 0u;
    std::vector<PackageValidationIssue> issues;
};

// Inspects a built product directory without needing the project. Verifies
// the player descriptor, runtime manifest, package headers, payload hashes,
// and that every manifest artifact resolves inside a shipped package.
NLS_BUILD_API bool InspectPackage(
    const std::filesystem::path& packageDirectory,
    PackageInspection& inspection,
    bool verifyPayloadHashes = true);

// Validates a ZIP archive by extracting it to a temporary directory and
// running InspectPackage. The archive must contain exactly one top-level
// directory.
NLS_BUILD_API bool ValidateArchive(
    const std::filesystem::path& archivePath,
    PackageInspection& inspection);

NLS_BUILD_API std::string FormatInspection(const PackageInspection& inspection);
} // namespace NLS::Build
