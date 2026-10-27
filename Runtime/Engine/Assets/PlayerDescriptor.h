#pragma once

#include "EngineDef.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Engine::Assets
{
// Descriptor written next to the executable of a packaged player. The runtime
// reads it before mounting content so it can verify that the player binary and
// the shipped packages come from the same build.
struct NLS_ENGINE_API PlayerScriptLayout
{
    std::string dotnetRoot;
    std::string managedRuntimeConfig;
    std::string managedAssembly;
    std::string scriptApiDirectory;
};

struct NLS_ENGINE_API PlayerDescriptor
{
    uint32_t schemaVersion = 1u;
    std::string playerId;
    std::string engineVersion;
    std::string targetId;
    std::string artifactPlatform;
    std::string graphicsBackend;
    std::string buildMode;
    std::string profileId;
    std::string productName;
    std::string companyName;
    std::string productVersion;
    std::string buildNumber;
    std::string startScene;
    // Runtime manifest shipped with the player ("Data/User/RuntimeAssetManifest.json").
    std::string runtimeManifestPath;
    // Content packages relative to the install root.
    std::vector<std::string> contentPackages;
    // Install-relative host root the content mounts use ("Data/User").
    std::string contentRoot = "Data/User";
    std::vector<std::string> scriptBackends;
    PlayerScriptLayout scripts;
    std::string buildInputHash;
    std::string buildReportPath;
    bool signedBuild = false;
    std::string signingDescription;

    static const char* FileName();

    std::string ToJson() const;
    static std::optional<PlayerDescriptor> FromJson(const std::string& json, std::string* error);
    bool Save(const std::filesystem::path& path, std::string* error) const;
    static std::optional<PlayerDescriptor> Load(const std::filesystem::path& path, std::string* error);
    // Looks for Player.info in the install root next to the executable.
    static std::optional<PlayerDescriptor> LoadForInstallRoot(
        const std::filesystem::path& installRoot,
        std::string* error);
};

struct PlayerCompatibilityIssue
{
    std::string code;
    std::string message;
};

// Verifies that the shipped packages match the running player. Returns an
// empty vector when the pair is compatible. `packageFormatVersion` is checked
// against the container version the player was built with.
NLS_ENGINE_API std::vector<PlayerCompatibilityIssue> CheckPlayerCompatibility(
    const PlayerDescriptor& descriptor,
    const std::string& engineVersion,
    const std::string& runtimeTargetId,
    uint32_t packageFormatVersion);
} // namespace NLS::Engine::Assets
