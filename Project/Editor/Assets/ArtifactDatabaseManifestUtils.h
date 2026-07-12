#pragma once

#include "Assets/ArtifactDatabase.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <sys/stat.h>
#endif

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
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
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(
        dataFilePath.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return {};

    FILE_BASIC_INFO basicInfo {};
    if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &basicInfo, sizeof(basicInfo)))
    {
        CloseHandle(handle);
        return {};
    }

    LARGE_INTEGER fileSize {};
    if (!GetFileSizeEx(handle, &fileSize))
    {
        CloseHandle(handle);
        return {};
    }

    FILE_ID_INFO fileId {};
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &fileId, sizeof(fileId)))
    {
        CloseHandle(handle);
        return std::to_string(fileSize.QuadPart) + ":" +
            std::to_string(basicInfo.LastWriteTime.QuadPart) + ":" +
            std::to_string(basicInfo.ChangeTime.QuadPart);
    }

    CloseHandle(handle);
    std::ostringstream stamp;
    stamp << "win:"
        << fileId.VolumeSerialNumber << ':';
    for (const auto byte : fileId.FileId.Identifier)
        stamp << static_cast<unsigned int>(byte) << '.';
    stamp << ':' << fileSize.QuadPart
        << ':' << basicInfo.LastWriteTime.QuadPart
        << ':' << basicInfo.ChangeTime.QuadPart;
    return stamp.str();
#else
    struct stat fileStat {};
    if (stat(dataFilePath.string().c_str(), &fileStat) != 0)
        return {};

#if defined(__APPLE__)
    const auto writeSeconds = static_cast<int64_t>(fileStat.st_mtimespec.tv_sec);
    const auto writeNanoseconds = static_cast<int64_t>(fileStat.st_mtimespec.tv_nsec);
    const auto changeSeconds = static_cast<int64_t>(fileStat.st_ctimespec.tv_sec);
    const auto changeNanoseconds = static_cast<int64_t>(fileStat.st_ctimespec.tv_nsec);
#else
    const auto writeSeconds = static_cast<int64_t>(fileStat.st_mtim.tv_sec);
    const auto writeNanoseconds = static_cast<int64_t>(fileStat.st_mtim.tv_nsec);
    const auto changeSeconds = static_cast<int64_t>(fileStat.st_ctim.tv_sec);
    const auto changeNanoseconds = static_cast<int64_t>(fileStat.st_ctim.tv_nsec);
#endif

    std::ostringstream stamp;
    stamp << "posix:"
        << static_cast<uint64_t>(fileStat.st_dev)
        << ':' << static_cast<uint64_t>(fileStat.st_ino)
        << ':' << static_cast<int64_t>(fileStat.st_size)
        << ':' << writeSeconds
        << ':' << writeNanoseconds
        << ':' << changeSeconds
        << ':' << changeNanoseconds;
    return stamp.str();
#endif
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
