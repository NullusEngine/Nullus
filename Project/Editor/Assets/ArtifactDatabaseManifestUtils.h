#pragma once

#include "Assets/ArtifactDatabase.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace NLS::Editor::Assets
{
inline std::filesystem::path GetProjectArtifactDatabasePath(const std::filesystem::path& projectRoot)
{
    return projectRoot / "Library" / "ArtifactDB";
}

inline std::mutex& GetProjectArtifactDatabaseManifestMutex(const std::filesystem::path& databasePath)
{
    static std::mutex registryMutex;
    static std::unordered_map<std::string, std::unique_ptr<std::mutex>> mutexByPath;

    std::lock_guard registryLock(registryMutex);
    auto& mutex = mutexByPath[databasePath.lexically_normal().generic_string()];
    if (!mutex)
        mutex = std::make_unique<std::mutex>();
    return *mutex;
}

inline std::string GetArtifactDatabaseDataFileStamp(const std::filesystem::path& databasePath)
{
    const auto dataFilePath = databasePath / "data.mdb";
    std::error_code error;
    const auto size = std::filesystem::file_size(dataFilePath, error);
    if (error)
        return {};

    error.clear();
    const auto writeTime = std::filesystem::last_write_time(dataFilePath, error);
    if (error)
        return {};

    return std::to_string(size) + ":" +
        std::to_string(static_cast<std::intmax_t>(writeTime.time_since_epoch().count()));
}

inline std::optional<NLS::Core::Assets::ArtifactManifest> LoadArtifactManifestFromProjectArtifactDB(
    const std::filesystem::path& projectRoot,
    const NLS::Core::Assets::AssetId sourceAssetId,
    const std::string& targetPlatform = "editor")
{
    if (!sourceAssetId.IsValid())
        return std::nullopt;

    const auto databasePath = GetProjectArtifactDatabasePath(projectRoot);
    const auto databaseKey = databasePath.lexically_normal().generic_string();
    const auto databaseStamp = GetArtifactDatabaseDataFileStamp(databasePath);
    std::lock_guard databaseLock(GetProjectArtifactDatabaseManifestMutex(databasePath));

    struct CachedArtifactDatabase
    {
        std::string stamp;
        std::shared_ptr<NLS::Core::Assets::ArtifactDatabase> database;
    };
    static std::mutex cacheMutex;
    static std::unordered_map<std::string, CachedArtifactDatabase> cacheByPath;

    std::lock_guard cacheLock(cacheMutex);
    auto& cached = cacheByPath[databaseKey];
    if (cached.stamp != databaseStamp)
    {
        auto reloaded = std::make_shared<NLS::Core::Assets::ArtifactDatabase>();
        if (databaseStamp.empty() || !reloaded->Load(databasePath))
        {
            cacheByPath.erase(databaseKey);
            return std::nullopt;
        }

        cached.stamp = databaseStamp;
        cached.database = std::move(reloaded);
    }

    if (cached.stamp.empty() || cached.database == nullptr)
        return std::nullopt;
    return cached.database->BuildManifestForSource(sourceAssetId, targetPlatform);
}

}
