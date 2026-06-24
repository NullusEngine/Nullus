#pragma once

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
    std::filesystem::path ResolveRuntimeArtifactDatabasePathForProjectSettings(
        const std::filesystem::path& projectSettingsPath);

    std::optional<NLS::Engine::Assets::RuntimeAssetDatabase> LoadRuntimeAssetDatabaseForProjectSettings(
        const std::filesystem::path& projectSettingsPath);

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
