#pragma once

#include "Engine/Assets/PlayerDescriptor.h"
#include "Engine/Assets/RuntimeAssetDatabase.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Core::ResourceManagement
{
    class MaterialManager;
}

namespace NLS::Game::RuntimeAssets
{
    // Describes a packaged player installation. When detected, the runtime
    // reads content exclusively from the shipped packages and manifest; it
    // never falls back to Library/ArtifactDB or the development project tree.
    struct PackagedRuntime
    {
        std::filesystem::path installRoot;
        std::filesystem::path contentRoot;      // <install>/Data/User
        std::filesystem::path packagesRoot;     // <install>/Data/User/Packages
        std::filesystem::path manifestPath;     // <install>/Data/User/RuntimeAssetManifest.json
        std::vector<std::filesystem::path> packages;
        NLS::Engine::Assets::PlayerDescriptor descriptor;
    };

    // Returns the packaged layout when <installRoot>/Data/User/Game.ini and a
    // player descriptor exist. Development runs return std::nullopt.
    std::optional<PackagedRuntime> DetectPackagedRuntime(const std::filesystem::path& installRoot);

    std::filesystem::path ResolveRuntimeArtifactDatabasePathForProjectSettings(
        const std::filesystem::path& projectSettingsPath);

    // Loads the runtime asset database. Packaged installs mount the shipped
    // .npak archives and read the manifest next to them; development runs keep
    // reading Library/ArtifactDB.
    std::optional<NLS::Engine::Assets::RuntimeAssetDatabase> LoadRuntimeAssetDatabaseForProjectSettings(
        const std::filesystem::path& projectSettingsPath,
        const PackagedRuntime* packagedRuntime = nullptr);

    // Validates Player.info against the running runtime before content loads.
    // Returns false and fills `error` when the player and packages disagree.
    bool ValidatePackagedRuntimeCompatibility(
        const PackagedRuntime& runtime,
        std::string* error);

    struct RuntimeMaterialPrewarmOptions
    {
        std::vector<NLS::Engine::Assets::RuntimeAssetRef> roots;
        std::vector<std::pair<std::string, std::string>> assetPacks;
    };

    size_t PrewarmRuntimeMaterialAssets(
        const NLS::Engine::Assets::RuntimeAssetDatabase& runtimeDatabase,
        NLS::Core::ResourceManagement::MaterialManager& materialManager,
        const RuntimeMaterialPrewarmOptions& options = {});
}
