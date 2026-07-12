#pragma once

#include "Assets/ArtifactManifest.h"
#include "Assets/NativeArtifactContainer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <Json/json.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
// SourceFileHashCache is included from editor headers; clean Win32 macros that
// collide with existing C++ method names in this codebase.
#ifdef GetObject
#undef GetObject
#endif
#ifdef GetMessage
#undef GetMessage
#endif
#ifdef CreateSemaphore
#undef CreateSemaphore
#endif
#endif

namespace NLS::Editor::Assets
{
inline constexpr uint32_t kSourceFileHashCacheSchemaVersion = 1u;

inline std::string BuildSourceFileHashCacheFileStamp(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return {};

    error.clear();
    const auto writeTime = std::filesystem::last_write_time(path, error);
    if (error)
        return {};

    const auto writeTimeTicks = static_cast<std::intmax_t>(writeTime.time_since_epoch().count());
    std::string stamp = "size:" + std::to_string(size) + "|mtime:" + std::to_string(writeTimeTicks);

#if defined(_WIN32)
    const auto handle = CreateFileW(
        path.wstring().c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle != INVALID_HANDLE_VALUE)
    {
        FILE_BASIC_INFO basicInfo {};
        if (GetFileInformationByHandleEx(handle, FileBasicInfo, &basicInfo, sizeof(basicInfo)))
        {
            stamp += "|ctime:" + std::to_string(basicInfo.ChangeTime.QuadPart);
        }

        FILE_ID_INFO fileIdInfo {};
        if (GetFileInformationByHandleEx(handle, FileIdInfo, &fileIdInfo, sizeof(fileIdInfo)))
        {
            stamp += "|volume:" + std::to_string(fileIdInfo.VolumeSerialNumber) + "|fileid:";
            for (const auto byte : fileIdInfo.FileId.Identifier)
                stamp += std::to_string(static_cast<unsigned int>(byte)) + ".";
        }
        CloseHandle(handle);
    }
#endif

    return stamp;
}

inline std::filesystem::path BuildSourceFileHashCachePath(const std::filesystem::path& projectRoot)
{
    return projectRoot / "Library" / "SourceFileHashCache" / "source-hashes.json";
}

inline std::string NormalizeSourceFileHashCacheKey(const std::filesystem::path& projectRoot, const std::filesystem::path& path)
{
    const auto normalizedPath = path.lexically_normal();
    if (!projectRoot.empty())
    {
        const auto relative = normalizedPath.lexically_relative(projectRoot.lexically_normal());
        if (!relative.empty() && !relative.is_absolute())
        {
            bool escapesRoot = false;
            for (const auto& part : relative)
            {
                if (part == "..")
                {
                    escapesRoot = true;
                    break;
                }
            }
            if (!escapesRoot)
                return relative.generic_string();
        }
    }
    return normalizedPath.generic_string();
}

struct SourceFileHashCacheEntry
{
    std::string stamp;
    std::string hash;
};

inline std::mutex& SourceFileHashCacheMutex()
{
    static std::mutex mutex;
    return mutex;
}

inline std::unordered_map<std::string, SourceFileHashCacheEntry>& SourceFileHashCacheEntries()
{
    static std::unordered_map<std::string, SourceFileHashCacheEntry> entries;
    return entries;
}

inline std::unordered_map<std::string, bool>& SourceFileHashCacheLoadedProjects()
{
    static std::unordered_map<std::string, bool> loadedProjects;
    return loadedProjects;
}

inline std::unordered_map<std::string, bool>& SourceFileHashCacheDirtyProjects()
{
    static std::unordered_map<std::string, bool> dirtyProjects;
    return dirtyProjects;
}

inline std::atomic<size_t>& SourceFileHashCacheContentHashReadCountStorage()
{
    static std::atomic<size_t> count = 0u;
    return count;
}

inline size_t GetSourceFileHashContentReadCount()
{
    return SourceFileHashCacheContentHashReadCountStorage().load(std::memory_order_relaxed);
}

inline std::string ReadSourceFileContentHashUncached(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return {};

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};

    std::vector<uint8_t> bytes;
    if (size <= static_cast<uintmax_t>(std::numeric_limits<size_t>::max()))
        bytes.reserve(static_cast<size_t>(size));

    std::vector<char> buffer(1024 * 1024);
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read = input.gcount();
        if (read <= 0)
            continue;

        const auto oldSize = bytes.size();
        bytes.resize(oldSize + static_cast<size_t>(read));
        std::memcpy(bytes.data() + oldSize, buffer.data(), static_cast<size_t>(read));
    }
    if (!input.eof())
        return {};

    SourceFileHashCacheContentHashReadCountStorage().fetch_add(1u, std::memory_order_relaxed);
    return NLS::Core::Assets::ComputeNativeArtifactPayloadHash(bytes);
}

inline std::string MakeSourceFileHashCacheStorageKey(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& path)
{
    return projectRoot.lexically_normal().generic_string() + "|" +
        NormalizeSourceFileHashCacheKey(projectRoot, path);
}

inline void LoadSourceFileHashCacheLocked(const std::filesystem::path& projectRoot)
{
    const auto projectKey = projectRoot.lexically_normal().generic_string();
    if (SourceFileHashCacheLoadedProjects()[projectKey])
        return;
    SourceFileHashCacheLoadedProjects()[projectKey] = true;

    std::ifstream input(BuildSourceFileHashCachePath(projectRoot), std::ios::binary);
    if (!input)
        return;

    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return;
    if (root.value("schema", 0u) != kSourceFileHashCacheSchemaVersion)
        return;

    const auto entries = root.find("entries");
    if (entries == root.end() || !entries->is_object())
        return;

    for (const auto& item : entries->items())
    {
        if (!item.value().is_object())
            continue;
        const auto stamp = item.value().value("stamp", std::string {});
        const auto hash = item.value().value("hash", std::string {});
        if (stamp.empty() || hash.empty())
            continue;

        SourceFileHashCacheEntries()[projectKey + "|" + item.key()] = {stamp, hash};
    }
}

inline void StoreSourceFileHashCacheLocked(const std::filesystem::path& projectRoot)
{
    const auto projectKey = projectRoot.lexically_normal().generic_string();
    nlohmann::json root = nlohmann::json::object();
    root["schema"] = kSourceFileHashCacheSchemaVersion;
    root["entries"] = nlohmann::json::object();

    const auto prefix = projectKey + "|";
    for (const auto& [key, entry] : SourceFileHashCacheEntries())
    {
        if (key.rfind(prefix, 0u) != 0u || entry.stamp.empty() || entry.hash.empty())
            continue;

        nlohmann::json item = nlohmann::json::object();
        item["stamp"] = entry.stamp;
        item["hash"] = entry.hash;
        root["entries"][key.substr(prefix.size())] = std::move(item);
    }

    const auto cachePath = BuildSourceFileHashCachePath(projectRoot);
    std::error_code error;
    std::filesystem::create_directories(cachePath.parent_path(), error);
    if (error)
        return;

    const auto tempPath = cachePath.string() + ".tmp";
    {
        std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
        if (!output)
            return;
        output << root.dump();
    }

    error.clear();
    std::filesystem::rename(tempPath, cachePath, error);
    if (error)
    {
        std::filesystem::remove(cachePath, error);
        error.clear();
        std::filesystem::rename(tempPath, cachePath, error);
    }
    if (!error)
        SourceFileHashCacheDirtyProjects()[projectKey] = false;
}

inline void RememberSourceFileContentHash(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& path,
    const std::string& contentHash,
    const bool persist = true)
{
    if (projectRoot.empty() || path.empty() || contentHash.empty())
        return;

    const auto stamp = BuildSourceFileHashCacheFileStamp(path);
    if (stamp.empty())
        return;

    std::lock_guard lock(SourceFileHashCacheMutex());
    LoadSourceFileHashCacheLocked(projectRoot);

    const auto projectKey = projectRoot.lexically_normal().generic_string();
    SourceFileHashCacheEntries()[MakeSourceFileHashCacheStorageKey(projectRoot, path)] = {stamp, contentHash};
    SourceFileHashCacheDirtyProjects()[projectKey] = true;
    if (persist)
        StoreSourceFileHashCacheLocked(projectRoot);
}

inline void FlushSourceFileHashCache(const std::filesystem::path& projectRoot)
{
    if (projectRoot.empty())
        return;

    std::lock_guard lock(SourceFileHashCacheMutex());
    const auto projectKey = projectRoot.lexically_normal().generic_string();
    if (!SourceFileHashCacheDirtyProjects()[projectKey])
        return;
    StoreSourceFileHashCacheLocked(projectRoot);
}

inline std::string CachedSourceFileContentHash(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& path,
    const bool persistOnMiss = true)
{
    if (projectRoot.empty() || path.empty())
        return ReadSourceFileContentHashUncached(path);

    const auto stamp = BuildSourceFileHashCacheFileStamp(path);
    if (stamp.empty())
        return {};

    {
        std::lock_guard lock(SourceFileHashCacheMutex());
        LoadSourceFileHashCacheLocked(projectRoot);

        const auto key = MakeSourceFileHashCacheStorageKey(projectRoot, path);
        const auto found = SourceFileHashCacheEntries().find(key);
        if (found != SourceFileHashCacheEntries().end() &&
            found->second.stamp == stamp &&
            !found->second.hash.empty())
        {
            return found->second.hash;
        }
    }

    const auto hash = ReadSourceFileContentHashUncached(path);
    if (hash.empty())
        return {};

    RememberSourceFileContentHash(projectRoot, path, hash, persistOnMiss);
    return hash;
}

inline void RememberManifestSourceFileHashes(
    const std::filesystem::path& projectRoot,
    const NLS::Core::Assets::ArtifactManifest& manifest)
{
    if (projectRoot.empty())
        return;

    bool changed = false;
    {
        std::lock_guard lock(SourceFileHashCacheMutex());
        LoadSourceFileHashCacheLocked(projectRoot);

        for (const auto& dependency : manifest.dependencies)
        {
            if (dependency.kind != NLS::Core::Assets::AssetDependencyKind::SourceFileHash ||
                dependency.value.empty() ||
                dependency.hashOrVersion.empty())
            {
                continue;
            }

            const auto path = (projectRoot / std::filesystem::path(dependency.value)).lexically_normal();
            const auto stamp = BuildSourceFileHashCacheFileStamp(path);
            if (stamp.empty())
                continue;

            const auto key = MakeSourceFileHashCacheStorageKey(projectRoot, path);
            const auto found = SourceFileHashCacheEntries().find(key);
            if (found != SourceFileHashCacheEntries().end() &&
                found->second.stamp == stamp &&
                found->second.hash == dependency.hashOrVersion)
            {
                continue;
            }

            SourceFileHashCacheEntries()[key] = {stamp, dependency.hashOrVersion};
            changed = true;
        }

        if (changed)
        {
            const auto projectKey = projectRoot.lexically_normal().generic_string();
            SourceFileHashCacheDirtyProjects()[projectKey] = true;
            StoreSourceFileHashCacheLocked(projectRoot);
        }
    }
}

#if defined(NLS_ENABLE_TEST_HOOKS)
inline void ClearSourceFileHashCacheForTesting()
{
    std::lock_guard lock(SourceFileHashCacheMutex());
    SourceFileHashCacheEntries().clear();
    SourceFileHashCacheLoadedProjects().clear();
    SourceFileHashCacheDirtyProjects().clear();
    SourceFileHashCacheContentHashReadCountStorage().store(0u, std::memory_order_relaxed);
}

inline size_t GetSourceFileHashContentReadCountForTesting()
{
    return GetSourceFileHashContentReadCount();
}
#endif
}
