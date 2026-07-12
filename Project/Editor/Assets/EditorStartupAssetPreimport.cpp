#include "Assets/EditorStartupAssetPreimport.h"

#include "Assets/AssetDatabaseFacade.h"
#include "Assets/ArtifactDatabaseManifestUtils.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetDatabase.h"
#include "Assets/EditorAssetPath.h"
#include "Assets/ExternalAssetImporter.h"
#include "Debug/Logger.h"
#include "Guid.h"
#include "Rendering/ShaderCompiler/ShaderCompiler.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <sys/stat.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NLS::Editor::Assets
{
namespace
{
constexpr const char* kProjectStandardPbrShaderPath = "Assets/Engine/Shaders/ShaderLab/StandardPBR.shader";
constexpr const char* kStartupAssetPreimportStampVersion = "8";
constexpr const char* kStartupAssetPreimportStampPath = "Library/Editor/StartupAssetPreimport.stamp";

struct PreparedPrefabCachePreflightSummary
{
    size_t attemptedCount = 0u;
    size_t preparedCount = 0u;
};

struct StartupAssetPreimportSourceEntry
{
    std::string rootMount;
    std::string relativePath;
    std::string stamp;
    std::string contentHash;
    std::string fingerprint;
    std::filesystem::path absolutePath;
};

struct StartupAssetPreimportArtifactEntry
{
    std::string relativePath;
    std::string stamp;
    std::string contentHash;
    std::string fingerprint;
};

struct StartupAssetPreimportDependencyEntry
{
    std::string ownerAssetPath;
    std::string relativePath;
    std::string stamp;
    std::string contentHash;
    std::string fingerprint;
};

struct StartupAssetPreimportDirectoryEntry
{
    std::string rootMount;
    std::string relativePath;
    std::string stamp;
    std::filesystem::path absolutePath;
};

struct StartupAssetPreimportIndex
{
    std::string projectRoot;
    std::string importerFingerprint;
    std::string artifactDatabaseStamp;
    std::vector<StartupAssetPreimportSourceEntry> sources;
    std::vector<StartupAssetPreimportDirectoryEntry> sourceDirectories;
    std::vector<StartupAssetPreimportDependencyEntry> dependencies;
    std::vector<StartupAssetPreimportArtifactEntry> artifacts;
};

struct StartupAssetPreimportCacheAnalysis
{
    bool cacheHit = false;
    std::optional<std::vector<std::filesystem::path>> changedSourcePaths;
    std::vector<std::filesystem::path> targetedRefreshSourceAssetPaths;
    std::vector<std::string> candidatePreimportAssetPaths;
    std::vector<std::filesystem::path> currentSourceAssetPaths;
    std::optional<StartupAssetPreimportIndex> loadedIndex;
    bool patchLoadedIndexOnCacheHit = false;
    StartupAssetPreimportCacheValidationProfile profile;
};

struct StartupFileMetadata
{
    std::string stamp;
    std::string fingerprint;
};

bool WriteStartupAssetPreimportIndex(
    const std::filesystem::path& stampPath,
    const StartupAssetPreimportIndex& index);

size_t& StartupAssetPreimportIndexLoadCountForTestingStorage()
{
    static size_t count = 0u;
    return count;
}

size_t& StartupAssetPreimportSourceEnumerationCountForTestingStorage()
{
    static size_t count = 0u;
    return count;
}

size_t& StartupAssetPreimportContentHashReadCountForTestingStorage()
{
    static size_t count = 0u;
    return count;
}

size_t& StartupAssetPreimportFileMetadataQueryCountForTestingStorage()
{
    static size_t count = 0u;
    return count;
}

size_t& StartupAssetPreimportShardWriteCountForTestingStorage()
{
    static size_t count = 0u;
    return count;
}

size_t& StartupAssetPreimportFullIndexRebuildCountForTestingStorage()
{
    static size_t count = 0u;
    return count;
}

size_t& StartupAssetPreimportPatchedIndexWriteCountForTestingStorage()
{
    static size_t count = 0u;
    return count;
}

size_t& StartupAssetPreimportImporterFingerprintComputeCountForTestingStorage()
{
    static size_t count = 0u;
    return count;
}

std::optional<StartupFileMetadata> FileMetadataForStartupCache(const std::filesystem::path& path)
{
    ++StartupAssetPreimportFileMetadataQueryCountForTestingStorage();
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::nullopt;

    FILE_BASIC_INFO basicInfo {};
    if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &basicInfo, sizeof(basicInfo)))
    {
        CloseHandle(handle);
        return std::nullopt;
    }

    LARGE_INTEGER fileSize {};
    if (!GetFileSizeEx(handle, &fileSize))
    {
        CloseHandle(handle);
        return std::nullopt;
    }

    FILE_ID_INFO fileId {};
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &fileId, sizeof(fileId)))
    {
        CloseHandle(handle);
        return StartupFileMetadata {
            std::to_string(fileSize.QuadPart) + ":" +
                std::to_string(basicInfo.LastWriteTime.QuadPart) + ":" +
                std::to_string(basicInfo.ChangeTime.QuadPart),
            {}
        };
    }

    CloseHandle(handle);

    std::ostringstream fingerprint;
    fingerprint << "win:"
        << std::hex << fileId.VolumeSerialNumber << ':';
    for (const auto byte : fileId.FileId.Identifier)
    {
        fingerprint << std::setw(2) << std::setfill('0')
            << static_cast<unsigned int>(byte);
    }
    fingerprint << std::dec
        << ':' << fileSize.QuadPart
        << ':' << basicInfo.LastWriteTime.QuadPart
        << ':' << basicInfo.ChangeTime.QuadPart;
    return StartupFileMetadata {
        std::to_string(fileSize.QuadPart) + ":" +
            std::to_string(basicInfo.LastWriteTime.QuadPart) + ":" +
            std::to_string(basicInfo.ChangeTime.QuadPart),
        fingerprint.str()
    };
#else
    struct stat fileStat {};
    if (stat(path.string().c_str(), &fileStat) != 0)
        return std::nullopt;

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
    stamp << static_cast<int64_t>(fileStat.st_size)
        << ':' << writeSeconds
        << ':' << writeNanoseconds
        << ':' << changeSeconds
        << ':' << changeNanoseconds;

    std::ostringstream fingerprint;
    fingerprint << "posix:"
        << static_cast<uint64_t>(fileStat.st_dev)
        << ':' << static_cast<uint64_t>(fileStat.st_ino)
        << ':' << static_cast<int64_t>(fileStat.st_size)
        << ':' << writeSeconds
        << ':' << writeNanoseconds
        << ':' << changeSeconds
        << ':' << changeNanoseconds;
    return StartupFileMetadata {stamp.str(), fingerprint.str()};
#endif
}

std::string FileStampForStartupCache(const std::filesystem::path& path)
{
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
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

    CloseHandle(handle);

    return std::to_string(fileSize.QuadPart) + ":" +
        std::to_string(basicInfo.LastWriteTime.QuadPart) + ":" +
        std::to_string(basicInfo.ChangeTime.QuadPart);
#else
    struct stat fileStat {};
    if (stat(path.string().c_str(), &fileStat) != 0)
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
    stamp << static_cast<int64_t>(fileStat.st_size)
        << ':' << writeSeconds
        << ':' << writeNanoseconds
        << ':' << changeSeconds
        << ':' << changeNanoseconds;
    return stamp.str();
#endif
}

std::optional<bool> FastFileMetadataStampMatchesStartupCache(
    const std::filesystem::path& path,
    const std::string& expectedStamp)
{
    constexpr std::string_view kFastStampPrefix = "fast:";
    if (expectedStamp.rfind(kFastStampPrefix, 0u) != 0u)
        return false;

#if defined(_WIN32)
    const HANDLE handle = CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::nullopt;

    FILE_BASIC_INFO basicInfo {};
    if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &basicInfo, sizeof(basicInfo)))
    {
        CloseHandle(handle);
        return std::nullopt;
    }

    LARGE_INTEGER fileSize {};
    if (!GetFileSizeEx(handle, &fileSize))
    {
        CloseHandle(handle);
        return std::nullopt;
    }
    CloseHandle(handle);

    const auto fastStamp =
        std::string(kFastStampPrefix) +
        std::to_string(fileSize.QuadPart) + ":" +
        std::to_string(basicInfo.LastWriteTime.QuadPart) + ":" +
        std::to_string(basicInfo.ChangeTime.QuadPart);
    return expectedStamp == fastStamp;
#else
    struct stat fileStat {};
    if (stat(path.string().c_str(), &fileStat) != 0)
        return std::nullopt;

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

    const auto fastStamp =
        std::string(kFastStampPrefix) +
        std::to_string(static_cast<int64_t>(fileStat.st_size)) + ":" +
        std::to_string(writeSeconds) + ":" +
        std::to_string(writeNanoseconds) + ":" +
        std::to_string(changeSeconds) + ":" +
        std::to_string(changeNanoseconds);
    return expectedStamp == fastStamp;
#endif
}

std::string FastFileMetadataStampForStartupCache(const std::filesystem::path& path)
{
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
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
    CloseHandle(handle);

    return "fast:" +
        std::to_string(fileSize.QuadPart) + ":" +
        std::to_string(basicInfo.LastWriteTime.QuadPart) + ":" +
        std::to_string(basicInfo.ChangeTime.QuadPart);
#else
    struct stat fileStat {};
    if (stat(path.string().c_str(), &fileStat) != 0)
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
    return "fast:" +
        std::to_string(static_cast<int64_t>(fileStat.st_size)) + ":" +
        std::to_string(writeSeconds) + ":" +
        std::to_string(writeNanoseconds) + ":" +
        std::to_string(changeSeconds) + ":" +
        std::to_string(changeNanoseconds);
#endif
}

std::string FileContentHashForStartupCache(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return {};
    ++StartupAssetPreimportContentHashReadCountForTestingStorage();

    uint64_t hash = 1469598103934665603ull;
    std::array<char, 64u * 1024u> buffer {};
    while (stream)
    {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto byteCount = stream.gcount();
        for (std::streamsize byteIndex = 0; byteIndex < byteCount; ++byteIndex)
        {
            hash ^= static_cast<uint8_t>(buffer[static_cast<size_t>(byteIndex)]);
            hash *= 1099511628211ull;
        }
    }
    if (stream.bad())
        return {};
    return "fnv1a64:" + std::to_string(hash);
}

bool ArtifactPayloadContentMatchesStartupCache(
    const std::filesystem::path& path,
    const std::string& expectedContentHash)
{
    constexpr std::string_view kSha256Prefix = "sha256:";
    if (expectedContentHash.rfind(kSha256Prefix, 0u) != 0u)
        return false;

    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return false;
    ++StartupAssetPreimportContentHashReadCountForTestingStorage();

    std::vector<uint8_t> bytes;
    std::array<char, 64u * 1024u> buffer {};
    while (stream)
    {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto byteCount = stream.gcount();
        if (byteCount > 0)
        {
            const auto oldSize = bytes.size();
            bytes.resize(oldSize + static_cast<size_t>(byteCount));
            std::memcpy(bytes.data() + oldSize, buffer.data(), static_cast<size_t>(byteCount));
        }
    }
    if (stream.bad())
        return false;

    return expectedContentHash == std::string(kSha256Prefix) +
        NLS::Core::Assets::BuildArtifactStorageFileName(bytes.data(), bytes.size());
}

std::string ResolveStartupContentHash(
    const std::filesystem::path& path,
    const std::string& currentFingerprint,
    const std::string& previousFingerprint,
    const std::string& previousContentHash)
{
    if (!currentFingerprint.empty() &&
        currentFingerprint == previousFingerprint &&
        !previousContentHash.empty())
    {
        return previousContentHash;
    }
    return FileContentHashForStartupCache(path);
}

std::string DirectoryStampForStartupCache(const std::filesystem::path& path)
{
    const auto metadata = FileMetadataForStartupCache(path);
    if (!metadata.has_value())
        return {};
    return metadata->fingerprint.empty() ? metadata->stamp : metadata->fingerprint;
}

std::string FormatStartupDuration(const std::chrono::steady_clock::duration duration)
{
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count()) + " ms";
}

void LogStartupTiming(
    std::string_view phase,
    const std::chrono::steady_clock::time_point begin,
    const size_t itemCount = 0u)
{
    std::ostringstream message;
    message << "[StartupAssetPreimport] " << phase
        << " took " << FormatStartupDuration(std::chrono::steady_clock::now() - begin);
    if (itemCount > 0u)
        message << " for " << itemCount << " items";
    NLS_LOG_INFO(message.str());
}

void LogStartupCacheValidationProfile(const StartupAssetPreimportCacheValidationProfile& profile)
{
    std::ostringstream message;
    message << "[StartupAssetPreimport] Cache validation profile:"
        << " sources=" << profile.sourceEntryCount
        << " sourceDirs=" << profile.sourceDirectoryEntryCount
        << " dependencies=" << profile.dependencyEntryCount
        << " artifacts=" << profile.artifactEntryCount
        << " trackedFiles=" << profile.trackedFileEntryCount
        << " metadataQueries=" << profile.fileMetadataQueryCount
        << " contentReads=" << profile.contentHashReadCount
        << " importerFingerprints=" << profile.importerFingerprintComputeCount
        << " elapsedMs=" << profile.elapsedMilliseconds;
    if (!profile.missReason.empty())
        message << " missReason=" << profile.missReason;
    NLS_LOG_INFO(message.str());
}

void LogStartupIndexPatchSkipped(std::string_view reason)
{
    NLS_LOG_INFO(std::string("[StartupAssetPreimport] Startup asset cache index patch skipped: ") + std::string(reason));
}

std::filesystem::path GetStartupAssetPreimportStampPath(const std::filesystem::path& projectRoot)
{
    return NLS::Core::Assets::NormalizeAssetPath(projectRoot) / kStartupAssetPreimportStampPath;
}

const std::array<NLS::Core::Assets::AssetType, 5u>& StartupPreimportAssetTypes()
{
    static constexpr std::array<NLS::Core::Assets::AssetType, 5u> kStartupPreimportAssetTypes = {
        NLS::Core::Assets::AssetType::ModelScene,
        NLS::Core::Assets::AssetType::Prefab,
        NLS::Core::Assets::AssetType::Material,
        NLS::Core::Assets::AssetType::Texture,
        NLS::Core::Assets::AssetType::Shader
    };
    return kStartupPreimportAssetTypes;
}

bool IsStartupPreimportAssetType(const NLS::Core::Assets::AssetType assetType)
{
    const auto& assetTypes = StartupPreimportAssetTypes();
    return std::find(assetTypes.begin(), assetTypes.end(), assetType) != assetTypes.end();
}

std::string StartupManifestTargetPlatformForAssetType(const NLS::Core::Assets::AssetType assetType)
{
    if (assetType == NLS::Core::Assets::AssetType::Texture)
    {
#if defined(_WIN32)
        return "win64-dx12";
#else
        return "editor";
#endif
    }
    return "editor";
}

std::string ComputeStartupImporterFingerprint()
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    ++StartupAssetPreimportImporterFingerprintComputeCountForTestingStorage();
#endif
    std::ostringstream stream;
    stream << "startup-manifest-freshness-v1;";
    for (const auto assetType : StartupPreimportAssetTypes())
    {
        const auto typeValue = static_cast<uint32_t>(assetType);
        stream << "importer:" << typeValue << '='
            << NLS::Core::Assets::GetCurrentImporterVersion(assetType)
            << ';';
        stream << "build-target:" << typeValue << '='
            << StartupManifestTargetPlatformForAssetType(assetType)
            << ';';
    }
    stream << "postprocessor:" << kExternalTextureBuildPipelineDependencyName << '='
        << kExternalTexturePostprocessorVersion
        << ';';
    stream << "postprocessor:shader-compiler-toolchain="
        << NLS::Render::ShaderCompiler::BuildShaderCompilerToolchainDependencyFingerprint()
        << ';';
    return stream.str();
}

std::string ToStartupEditorAssetPath(const StartupAssetPreimportSourceEntry& source)
{
    auto path = std::filesystem::path(source.rootMount) / source.relativePath;
    return NormalizeEditorAssetPath(path);
}

std::string StartupSourceEntryKey(
    const std::string_view rootMount,
    const std::string_view relativePath)
{
    std::string key(rootMount);
    key.push_back('|');
    key.append(relativePath);
    return key;
}

std::string StartupDirectoryEntryKey(
    const std::string_view rootMount,
    const std::string_view relativePath)
{
    return StartupSourceEntryKey(rootMount, relativePath);
}

bool ShouldIgnoreStartupSourceTimestampOnlyChange(
    const std::string_view rootMount,
    const std::string_view relativePath)
{
    return NormalizeEditorAssetPath(std::filesystem::path(rootMount) / std::filesystem::path(relativePath)) ==
        kProjectStandardPbrShaderPath;
}

struct StartupManifestFreshnessFingerprint
{
    std::string namespaceTag;
    std::unordered_map<std::string, std::string> values;
};

std::optional<NLS::Core::Assets::AssetType> TryParseStartupManifestFreshnessAssetType(const std::string_view key)
{
    constexpr std::string_view kImporterPrefix = "importer:";
    constexpr std::string_view kBuildTargetPrefix = "build-target:";
    std::string_view payload;
    if (key.rfind(kImporterPrefix, 0u) == 0u)
        payload = key.substr(kImporterPrefix.size());
    else if (key.rfind(kBuildTargetPrefix, 0u) == 0u)
        payload = key.substr(kBuildTargetPrefix.size());
    else
        return std::nullopt;

    uint32_t rawValue = 0u;
    std::istringstream stream {std::string(payload)};
    stream >> rawValue;
    if (stream.fail())
        return std::nullopt;

    const auto assetType = static_cast<NLS::Core::Assets::AssetType>(rawValue);
    return IsStartupPreimportAssetType(assetType)
        ? std::optional<NLS::Core::Assets::AssetType> {assetType}
        : std::nullopt;
}

std::optional<StartupManifestFreshnessFingerprint> ParseStartupManifestFreshnessFingerprint(const std::string& text)
{
    StartupManifestFreshnessFingerprint fingerprint;
    std::stringstream stream(text);
    std::string token;
    while (std::getline(stream, token, ';'))
    {
        if (token.empty())
            continue;
        if (fingerprint.namespaceTag.empty())
        {
            fingerprint.namespaceTag = token;
            continue;
        }

        const auto separator = token.find('=');
        if (separator == std::string::npos || separator == 0u || separator + 1u >= token.size())
            return std::nullopt;
        fingerprint.values.emplace(token.substr(0u, separator), token.substr(separator + 1u));
    }

    return fingerprint.namespaceTag.empty()
        ? std::nullopt
        : std::optional<StartupManifestFreshnessFingerprint> {std::move(fingerprint)};
}

std::optional<std::vector<NLS::Core::Assets::AssetType>> TryGetStartupManifestFreshnessAffectedAssetTypes(
    const std::string& previousFingerprint,
    const std::string& currentFingerprint)
{
    const auto previous = ParseStartupManifestFreshnessFingerprint(previousFingerprint);
    const auto current = ParseStartupManifestFreshnessFingerprint(currentFingerprint);
    if (!previous.has_value() || !current.has_value() || previous->namespaceTag != current->namespaceTag)
        return std::nullopt;

    std::vector<NLS::Core::Assets::AssetType> affectedTypes;
    std::unordered_set<NLS::Core::Assets::AssetType> uniqueAffectedTypes;
    std::unordered_set<std::string> keys;
    for (const auto& [key, _] : previous->values)
        keys.insert(key);
    for (const auto& [key, _] : current->values)
        keys.insert(key);

    for (const auto& key : keys)
    {
        const auto previousValue = previous->values.find(key);
        const auto currentValue = current->values.find(key);
        const std::string_view previousText = previousValue != previous->values.end() ? previousValue->second : std::string_view {};
        const std::string_view currentText = currentValue != current->values.end() ? currentValue->second : std::string_view {};
        if (previousText == currentText)
            continue;

        if (const auto assetType = TryParseStartupManifestFreshnessAssetType(key); assetType.has_value())
        {
            if (uniqueAffectedTypes.insert(*assetType).second)
                affectedTypes.push_back(*assetType);
            continue;
        }

        if (key == std::string("postprocessor:") + kExternalTextureBuildPipelineDependencyName)
        {
            if (uniqueAffectedTypes.insert(NLS::Core::Assets::AssetType::Texture).second)
                affectedTypes.push_back(NLS::Core::Assets::AssetType::Texture);
            continue;
        }

        if (key == "postprocessor:shader-compiler-toolchain")
        {
            if (uniqueAffectedTypes.insert(NLS::Core::Assets::AssetType::Shader).second)
                affectedTypes.push_back(NLS::Core::Assets::AssetType::Shader);
            continue;
        }

        return std::nullopt;
    }

    return affectedTypes;
}

std::filesystem::path ResolveStartupRootMountedPath(
    const std::vector<EditorAssetRoot>& roots,
    const std::string& rootMount,
    const std::string& relativePath)
{
    const auto normalizedRootMount = NormalizeEditorAssetPath(rootMount);
    const auto normalizedRelativePath = relativePath.empty()
        ? std::string {}
        : NormalizeEditorAssetPath(relativePath);
    for (const auto& root : roots)
    {
        if (NormalizeEditorAssetPath(root.mountPath) != normalizedRootMount)
            continue;

        std::error_code error;
        if (std::filesystem::is_regular_file(root.path, error) && !error)
        {
            if (normalizedRelativePath.empty() ||
                normalizedRelativePath == NormalizeEditorAssetPath(root.path.filename()))
            {
                const auto normalizedPath = NLS::Core::Assets::NormalizeAssetPath(root.path);
                if (normalizedPath.empty())
                    return {};
                return normalizedPath;
            }
            return {};
        }

        const auto normalizedPath = NLS::Core::Assets::NormalizeAssetPath(root.path / normalizedRelativePath);
        if (normalizedPath.empty())
            return {};
        return normalizedPath;
    }
    return {};
}

bool IsStartupPreimportSourcePath(const std::filesystem::path& editorAssetPath)
{
    auto normalized = NormalizeEditorAssetPath(editorAssetPath);
    constexpr std::string_view kMetaSuffix = ".meta";
    if (normalized.size() > kMetaSuffix.size() &&
        normalized.compare(normalized.size() - kMetaSuffix.size(), kMetaSuffix.size(), kMetaSuffix) == 0)
    {
        normalized.resize(normalized.size() - kMetaSuffix.size());
    }
    return IsStartupPreimportAssetType(NLS::Core::Assets::InferAssetType(normalized));
}

bool IsStartupMetaPath(const std::string& assetPath)
{
    constexpr std::string_view kMetaSuffix = ".meta";
    return assetPath.size() > kMetaSuffix.size() &&
        assetPath.compare(assetPath.size() - kMetaSuffix.size(), kMetaSuffix.size(), kMetaSuffix) == 0;
}

bool IsStartupManifestFreshnessAffectedSourcePath(
    const std::string& editorAssetPath,
    const std::vector<NLS::Core::Assets::AssetType>& affectedTypes)
{
    if (affectedTypes.empty())
        return false;

    auto normalizedPath = NormalizeEditorAssetPath(editorAssetPath);
    if (IsStartupMetaPath(normalizedPath))
        normalizedPath.resize(normalizedPath.size() - 5u);
    const auto assetType = NLS::Core::Assets::InferAssetType(normalizedPath);
    return std::find(affectedTypes.begin(), affectedTypes.end(), assetType) != affectedTypes.end();
}

std::vector<EditorAssetRoot> MakeStartupSourceRoots(const std::filesystem::path& projectRoot);

std::filesystem::path NormalizeStartupPreimportCandidatePath(const std::filesystem::path& path)
{
    return std::filesystem::path(NormalizeEditorAssetPath(path)).lexically_normal();
}

bool StartupPathIsOrContainsPath(
    const std::filesystem::path& changedPath,
    const std::filesystem::path& candidatePath)
{
    const auto changed = NormalizeStartupPreimportCandidatePath(changedPath);
    const auto candidate = NormalizeStartupPreimportCandidatePath(candidatePath);
    if (changed.empty())
        return true;
    if (changed == candidate)
        return true;

    const auto relative = candidate.lexically_relative(changed);
    if (relative.empty() || relative.is_absolute())
        return false;
    for (const auto& part : relative)
    {
        if (part == "..")
            return false;
    }
    return true;
}

std::vector<std::string> BuildStartupPreimportCandidateAssetPaths(
    const StartupAssetPreimportIndex& index,
    const std::vector<std::filesystem::path>& changedPaths,
    const bool includeDependencyOwners)
{
    std::vector<std::string> candidates;
    for (const auto& changedPath : changedPaths)
    {
        auto normalizedChangedPath = NormalizeEditorAssetPath(changedPath);
        if (IsStartupMetaPath(normalizedChangedPath))
            normalizedChangedPath.resize(normalizedChangedPath.size() - 5u);
        if (IsStartupPreimportAssetType(NLS::Core::Assets::InferAssetType(normalizedChangedPath)))
            candidates.push_back(normalizedChangedPath);

        if (!includeDependencyOwners)
            continue;

        for (const auto& dependency : index.dependencies)
        {
            if (StartupPathIsOrContainsPath(changedPath, dependency.relativePath))
                candidates.push_back(NormalizeEditorAssetPath(dependency.ownerAssetPath));
        }
    }

    candidates.erase(
        std::remove_if(
            candidates.begin(),
            candidates.end(),
            [](const std::string& assetPath)
            {
                return assetPath.empty() ||
                    !IsStartupPreimportAssetType(NLS::Core::Assets::InferAssetType(assetPath));
            }),
        candidates.end());
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    return candidates;
}

std::vector<std::filesystem::path> ResolveStartupChangedSourceAssetPaths(
    const std::filesystem::path& projectRoot,
    const std::vector<std::filesystem::path>& changedPaths)
{
    std::vector<std::filesystem::path> absolutePaths;
    const auto roots = MakeStartupSourceRoots(projectRoot);
    for (const auto& changedPath : changedPaths)
    {
        auto normalizedChangedPath = NormalizeEditorAssetPath(changedPath);
        if (IsStartupMetaPath(normalizedChangedPath))
            normalizedChangedPath.resize(normalizedChangedPath.size() - 5u);
        if (!IsStartupPreimportAssetType(NLS::Core::Assets::InferAssetType(normalizedChangedPath)))
            continue;

        std::filesystem::path fallbackPath;
        for (const auto& root : roots)
        {
            const auto mountPath = NormalizeEditorAssetPath(root.mountPath);
            std::string relativePath;
            std::error_code error;
            if (std::filesystem::is_regular_file(root.path, error) && !error)
            {
                const auto rootEditorPath = NormalizeEditorAssetPath(
                    std::filesystem::path(mountPath) / root.path.filename());
                if (rootEditorPath != normalizedChangedPath)
                    continue;
                relativePath = NormalizeEditorAssetPath(root.path.filename());
            }
            else
            {
                const auto mountPrefix = mountPath + "/";
                if (normalizedChangedPath.rfind(mountPrefix, 0u) != 0u)
                    continue;
                relativePath = NormalizeEditorAssetPath(normalizedChangedPath.substr(mountPrefix.size()));
            }

            const auto absolutePath = ResolveStartupRootMountedPath(roots, mountPath, relativePath);
            if (absolutePath.empty())
                continue;

            if (fallbackPath.empty())
                fallbackPath = absolutePath;

            error.clear();
            if (std::filesystem::is_regular_file(absolutePath, error) && !error)
            {
                absolutePaths.push_back(absolutePath);
                fallbackPath.clear();
                break;
            }
        }

        if (!fallbackPath.empty())
        {
            absolutePaths.push_back(fallbackPath);
        }
    }

    std::sort(absolutePaths.begin(), absolutePaths.end());
    absolutePaths.erase(std::unique(absolutePaths.begin(), absolutePaths.end()), absolutePaths.end());
    return absolutePaths;
}

std::vector<EditorAssetRoot> MakeStartupSourceRoots(const std::filesystem::path& projectRoot)
{
    std::vector<EditorAssetRoot> startupRoots;
    startupRoots.push_back({
        projectRoot / "Assets",
        false,
        "Assets",
        projectRoot / "Library"
    });

    const auto roots = MakeProjectEditorAssetRoots(projectRoot);
    for (const auto& root : roots)
    {
        if (NormalizeEditorAssetPath(root.mountPath) == "Assets/Engine/Shaders")
        {
            startupRoots.push_back({
                root.path / "ShaderLab" / "StandardPBR.shader",
                true,
                root.mountPath / "ShaderLab",
                root.libraryPath
            });
        }
    }
    return startupRoots;
}

void AddStartupDirectoryEntry(
    std::vector<StartupAssetPreimportDirectoryEntry>& directories,
    const EditorAssetRoot& root,
    const std::filesystem::path& directory)
{
    const auto stamp = DirectoryStampForStartupCache(directory);
    if (stamp.empty())
        return;

    auto relativePath = directory.lexically_relative(root.path);
    if (relativePath.empty() || relativePath == ".")
        relativePath.clear();
    directories.push_back({
        NormalizeEditorAssetPath(root.mountPath),
        NormalizeEditorAssetPath(relativePath),
        stamp,
        directory
    });
}

void CollectStartupSourceEntriesForRoot(
    std::vector<StartupAssetPreimportSourceEntry>& sources,
    std::vector<StartupAssetPreimportDirectoryEntry>* sourceDirectories,
    const EditorAssetRoot& root,
    const std::unordered_map<std::string, StartupAssetPreimportSourceEntry>* previousByKey)
{
    std::error_code error;
    if (!std::filesystem::exists(root.path, error) || error)
        return;

    std::vector<std::filesystem::path> paths;
    if (std::filesystem::is_regular_file(root.path, error) && !error)
    {
        paths.push_back(root.path);
    }
    else
    {
        if (sourceDirectories)
            AddStartupDirectoryEntry(*sourceDirectories, root, root.path);
#if defined(NLS_ENABLE_TEST_HOOKS)
        ++StartupAssetPreimportSourceEnumerationCountForTestingStorage();
#endif
        error.clear();
        for (std::filesystem::recursive_directory_iterator iterator(
                 root.path,
                 std::filesystem::directory_options::skip_permission_denied,
                 error);
             !error && iterator != std::filesystem::recursive_directory_iterator();
             iterator.increment(error))
        {
            const auto& entry = *iterator;
            if (entry.is_regular_file(error))
                paths.push_back(entry.path());
            else if (sourceDirectories && entry.is_directory(error))
                AddStartupDirectoryEntry(*sourceDirectories, root, entry.path());
            error.clear();
        }
    }

    std::sort(paths.begin(), paths.end());
    const auto mountPath = NormalizeEditorAssetPath(root.mountPath);
    for (const auto& path : paths)
    {
        auto relativePath = path.lexically_relative(root.path);
        if (relativePath.empty() || relativePath == ".")
            relativePath = path.filename();
        const auto relativeEditorPath = NormalizeEditorAssetPath(relativePath);
        const auto editorAssetPath = NormalizeEditorAssetPath(std::filesystem::path(mountPath) / relativeEditorPath);
        if (!IsStartupPreimportSourcePath(editorAssetPath))
            continue;

        const auto metadata = FileMetadataForStartupCache(path);
        if (!metadata.has_value())
            continue;

        std::string contentHash;
        if (previousByKey)
        {
            const auto previous = previousByKey->find(StartupSourceEntryKey(mountPath, relativeEditorPath));
            if (previous == previousByKey->end() || previous->second.stamp != metadata->stamp)
            {
                sources.push_back({
                    mountPath,
                    relativeEditorPath,
                    metadata->stamp,
                    {},
                    metadata->fingerprint,
                    path
                });
                continue;
            }
            contentHash = ResolveStartupContentHash(
                path,
                metadata->fingerprint,
                previous->second.fingerprint,
                previous->second.contentHash);
        }
        else
        {
            contentHash = FileContentHashForStartupCache(path);
        }

        if (contentHash.empty())
            continue;
        sources.push_back({
            mountPath,
            relativeEditorPath,
            metadata->stamp,
            contentHash,
            metadata->fingerprint,
            path
        });
    }
}

struct StartupSourceCollection
{
    std::vector<StartupAssetPreimportSourceEntry> sources;
    std::vector<StartupAssetPreimportDirectoryEntry> sourceDirectories;
};

std::optional<StartupSourceCollection> CollectStartupSourceEntries(
    const std::filesystem::path& projectRoot,
    const std::unordered_map<std::string, StartupAssetPreimportSourceEntry>* previousByKey = nullptr)
{
    const auto normalizedProjectRoot = NLS::Core::Assets::NormalizeAssetPath(projectRoot);
    if (normalizedProjectRoot.empty() || normalizedProjectRoot == normalizedProjectRoot.root_path())
        return std::nullopt;

    StartupSourceCollection collection;
    for (const auto& root : MakeStartupSourceRoots(normalizedProjectRoot))
        CollectStartupSourceEntriesForRoot(collection.sources, &collection.sourceDirectories, root, previousByKey);

    std::sort(
        collection.sources.begin(),
        collection.sources.end(),
        [](const auto& lhs, const auto& rhs)
        {
            return std::tie(lhs.rootMount, lhs.relativePath, lhs.stamp, lhs.contentHash) <
                std::tie(rhs.rootMount, rhs.relativePath, rhs.stamp, rhs.contentHash);
        });
    std::sort(
        collection.sourceDirectories.begin(),
        collection.sourceDirectories.end(),
        [](const auto& lhs, const auto& rhs)
        {
            return std::tie(lhs.rootMount, lhs.relativePath, lhs.stamp) <
                std::tie(rhs.rootMount, rhs.relativePath, rhs.stamp);
        });
    return collection;
}

bool StartupSourceDirectoriesMatchIndex(
    const std::vector<EditorAssetRoot>& roots,
    const StartupAssetPreimportIndex& index)
{
    if (index.sourceDirectories.empty())
        return false;

    for (const auto& directory : index.sourceDirectories)
    {
        const auto absolutePath = ResolveStartupRootMountedPath(roots, directory.rootMount, directory.relativePath);
        if (absolutePath.empty())
            return false;
        const auto stamp = DirectoryStampForStartupCache(absolutePath);
        if (stamp.empty() || stamp != directory.stamp)
            return false;
    }
    return true;
}

std::optional<StartupSourceCollection> TryCollectStartupSourceEntriesFromIndex(
    const std::filesystem::path& projectRoot,
    const StartupAssetPreimportIndex& index,
    const std::unordered_map<std::string, StartupAssetPreimportSourceEntry>& previousByKey)
{
    const auto roots = MakeStartupSourceRoots(projectRoot);
    if (!StartupSourceDirectoriesMatchIndex(roots, index))
        return std::nullopt;

    StartupSourceCollection collection;
    collection.sourceDirectories = index.sourceDirectories;
    collection.sources.reserve(index.sources.size());
    for (const auto& previousSource : index.sources)
    {
        const auto rootMount = NormalizeEditorAssetPath(previousSource.rootMount);
        const auto relativePath = NormalizeEditorAssetPath(previousSource.relativePath);
        const auto absolutePath = ResolveStartupRootMountedPath(roots, rootMount, relativePath);
        if (absolutePath.empty())
            return std::nullopt;

        const auto currentStamp = FileStampForStartupCache(absolutePath);
        if (currentStamp.empty())
            return std::nullopt;

        std::string contentHash;
        const auto previous = previousByKey.find(StartupSourceEntryKey(rootMount, relativePath));
        if (previous != previousByKey.end() &&
            previous->second.stamp == currentStamp &&
            !previous->second.contentHash.empty())
        {
            collection.sources.push_back({
                rootMount,
                relativePath,
                previous->second.stamp,
                previous->second.contentHash,
                previous->second.fingerprint,
                absolutePath
            });
            continue;
        }

        const auto metadata = FileMetadataForStartupCache(absolutePath);
        if (!metadata.has_value())
            return std::nullopt;

        if (previous == previousByKey.end() || previous->second.stamp != metadata->stamp)
        {
            if (previous != previousByKey.end() &&
                ShouldIgnoreStartupSourceTimestampOnlyChange(rootMount, relativePath))
            {
                contentHash = ResolveStartupContentHash(
                    absolutePath,
                    metadata->fingerprint,
                    previous->second.fingerprint,
                    previous->second.contentHash);
                if (contentHash.empty())
                    return std::nullopt;
                if (contentHash == previous->second.contentHash)
                {
                    collection.sources.push_back({
                        rootMount,
                        relativePath,
                        previous->second.stamp,
                        previous->second.contentHash,
                        previous->second.fingerprint,
                        absolutePath
                    });
                    continue;
                }
            }
            collection.sources.push_back({
                rootMount,
                relativePath,
                metadata->stamp,
                {},
                metadata->fingerprint,
                absolutePath
            });
            continue;
        }

        contentHash = ResolveStartupContentHash(
            absolutePath,
            metadata->fingerprint,
            previous->second.fingerprint,
            previous->second.contentHash);
        if (contentHash.empty())
            return std::nullopt;
        collection.sources.push_back({
            rootMount,
            relativePath,
            metadata->stamp,
            contentHash,
            metadata->fingerprint,
            absolutePath
        });
    }

    std::sort(
        collection.sources.begin(),
        collection.sources.end(),
        [](const auto& lhs, const auto& rhs)
        {
            return std::tie(lhs.rootMount, lhs.relativePath, lhs.stamp, lhs.contentHash) <
                std::tie(rhs.rootMount, rhs.relativePath, rhs.stamp, rhs.contentHash);
        });
    return collection;
}

std::filesystem::path ResolveStartupArtifactPath(
    const std::filesystem::path& projectRoot,
    const std::string& artifactPath)
{
    const auto path = NLS::Core::Assets::NormalizeAssetPath(artifactPath);
    if (path.empty())
        return {};

    const auto artifactRoot = NLS::Core::Assets::NormalizeAssetPath(projectRoot / "Library" / "Artifacts");
    if (artifactRoot.empty())
        return {};
    if (!path.is_absolute())
        return {};

    const auto relative = path.lexically_relative(artifactRoot);
    if (relative.empty() || relative.is_absolute())
        return {};
    for (const auto& part : relative)
    {
        if (part == "..")
            return {};
    }
    return path;
}

bool SourceEntriesEqual(
    const std::vector<StartupAssetPreimportSourceEntry>& lhs,
    const std::vector<StartupAssetPreimportSourceEntry>& rhs)
{
    if (lhs.size() != rhs.size())
        return false;
    for (size_t index = 0u; index < lhs.size(); ++index)
    {
        if (lhs[index].rootMount != rhs[index].rootMount ||
            lhs[index].relativePath != rhs[index].relativePath ||
            lhs[index].stamp != rhs[index].stamp ||
            lhs[index].contentHash != rhs[index].contentHash)
        {
            return false;
        }
    }
    return true;
}

bool ArtifactEntriesEqual(
    const std::vector<StartupAssetPreimportArtifactEntry>& lhs,
    const std::vector<StartupAssetPreimportArtifactEntry>& rhs)
{
    if (lhs.size() != rhs.size())
        return false;
    for (size_t index = 0u; index < lhs.size(); ++index)
    {
        if (lhs[index].relativePath != rhs[index].relativePath ||
            lhs[index].stamp != rhs[index].stamp ||
            lhs[index].contentHash != rhs[index].contentHash)
        {
            return false;
        }
    }
    return true;
}

bool DependencyEntriesEqual(
    const std::vector<StartupAssetPreimportDependencyEntry>& lhs,
    const std::vector<StartupAssetPreimportDependencyEntry>& rhs)
{
    if (lhs.size() != rhs.size())
        return false;
    for (size_t index = 0u; index < lhs.size(); ++index)
    {
        if (lhs[index].ownerAssetPath != rhs[index].ownerAssetPath ||
            lhs[index].relativePath != rhs[index].relativePath ||
            lhs[index].stamp != rhs[index].stamp ||
            lhs[index].contentHash != rhs[index].contentHash)
        {
            return false;
        }
    }
    return true;
}

std::string SourceEntryKey(const StartupAssetPreimportSourceEntry& source)
{
    return StartupSourceEntryKey(source.rootMount, source.relativePath);
}

bool SourceEntryMetadataEqual(
    const StartupAssetPreimportSourceEntry& lhs,
    const StartupAssetPreimportSourceEntry& rhs)
{
    return lhs.rootMount == rhs.rootMount &&
        lhs.relativePath == rhs.relativePath &&
        lhs.stamp == rhs.stamp &&
        lhs.contentHash == rhs.contentHash;
}

void SortStartupArtifactEntries(std::vector<StartupAssetPreimportArtifactEntry>& artifacts)
{
    std::sort(
        artifacts.begin(),
        artifacts.end(),
        [](const auto& lhs, const auto& rhs)
        {
            return std::tie(lhs.relativePath, lhs.stamp, lhs.contentHash) <
                std::tie(rhs.relativePath, rhs.stamp, rhs.contentHash);
        });
}

void SortStartupDependencyEntries(std::vector<StartupAssetPreimportDependencyEntry>& dependencies)
{
    std::sort(
        dependencies.begin(),
        dependencies.end(),
        [](const auto& lhs, const auto& rhs)
        {
            return std::tie(lhs.ownerAssetPath, lhs.relativePath, lhs.stamp, lhs.contentHash) <
                std::tie(rhs.ownerAssetPath, rhs.relativePath, rhs.stamp, rhs.contentHash);
        });
}

std::filesystem::path ResolveStartupDependencyPath(
    const std::filesystem::path& projectRoot,
    const std::string& dependencyPath)
{
    const auto normalized = std::filesystem::path(NormalizeEditorAssetPath(dependencyPath));
    if (normalized.empty() || normalized.is_absolute())
        return {};
    for (const auto& part : normalized)
    {
        if (part == "..")
            return {};
    }
    return NLS::Core::Assets::NormalizeAssetPath(projectRoot / normalized);
}

std::vector<StartupAssetPreimportDependencyEntry> CollectStartupDependencyEntriesFromIndex(
    const std::filesystem::path& projectRoot,
    const StartupAssetPreimportIndex& index)
{
    std::vector<StartupAssetPreimportDependencyEntry> dependencies;
    dependencies.reserve(index.dependencies.size());
    for (const auto& dependency : index.dependencies)
    {
        const auto absolutePath = ResolveStartupDependencyPath(projectRoot, dependency.relativePath);
        const auto currentStamp = FileStampForStartupCache(absolutePath);
        if (currentStamp == dependency.stamp)
        {
            dependencies.push_back({
                dependency.ownerAssetPath,
                NormalizeEditorAssetPath(dependency.relativePath),
                dependency.stamp,
                dependency.contentHash,
                dependency.fingerprint
            });
            continue;
        }

        const auto metadata = FileMetadataForStartupCache(absolutePath);
        if (!metadata.has_value())
            continue;
        if (metadata->stamp != dependency.stamp)
        {
            dependencies.push_back({
                dependency.ownerAssetPath,
                NormalizeEditorAssetPath(dependency.relativePath),
                metadata->stamp,
                {},
                {}
            });
            continue;
        }

        const auto contentHash = ResolveStartupContentHash(
            absolutePath,
            metadata->fingerprint,
            dependency.fingerprint,
            dependency.contentHash);
        if (contentHash.empty())
            continue;
        dependencies.push_back({
            dependency.ownerAssetPath,
            NormalizeEditorAssetPath(dependency.relativePath),
            metadata->stamp,
            contentHash,
            metadata->fingerprint
        });
    }
    SortStartupDependencyEntries(dependencies);
    return dependencies;
}

void AppendStartupDependencyEntries(
    std::vector<StartupAssetPreimportDependencyEntry>& dependencies,
    const std::filesystem::path& projectRoot,
    const std::string& ownerAssetPath,
    const NLS::Core::Assets::ArtifactManifest& manifest)
{
    for (const auto& dependency : manifest.dependencies)
    {
        if (dependency.kind != NLS::Core::Assets::AssetDependencyKind::SourceFileHash)
            continue;

        const auto relativePath = NormalizeEditorAssetPath(dependency.value);
        if (relativePath.empty() || relativePath == NormalizeEditorAssetPath(ownerAssetPath))
            continue;

        const auto absolutePath = ResolveStartupDependencyPath(projectRoot, relativePath);
        const auto metadata = FileMetadataForStartupCache(absolutePath);
        const auto contentHash = FileContentHashForStartupCache(absolutePath);
        if (!metadata.has_value() || contentHash.empty())
            continue;

        dependencies.push_back({
            NormalizeEditorAssetPath(ownerAssetPath),
            relativePath,
            metadata->stamp,
            contentHash,
            metadata->fingerprint
        });
    }
}

std::optional<StartupAssetPreimportIndex> BuildStartupAssetPreimportIndex(
    const std::filesystem::path& projectRoot,
    AssetDatabaseFacade& database,
    const StartupAssetPreimportIndex* previousIndex = nullptr)
{
    const auto normalizedProjectRoot = NLS::Core::Assets::NormalizeAssetPath(projectRoot);
    if (normalizedProjectRoot.empty() || normalizedProjectRoot == normalizedProjectRoot.root_path())
        return std::nullopt;

    std::unordered_map<std::string, StartupAssetPreimportSourceEntry> previousSourcesByKey;
    if (previousIndex != nullptr)
    {
        previousSourcesByKey.reserve(previousIndex->sources.size());
        for (const auto& source : previousIndex->sources)
            previousSourcesByKey.emplace(SourceEntryKey(source), source);
    }

    auto sources = CollectStartupSourceEntries(
        normalizedProjectRoot,
        previousIndex != nullptr ? &previousSourcesByKey : nullptr);
    if (!sources.has_value())
        return std::nullopt;

    StartupAssetPreimportIndex index;
    index.projectRoot = normalizedProjectRoot.generic_string();
    index.importerFingerprint = ComputeStartupImporterFingerprint();
    index.artifactDatabaseStamp = GetArtifactDatabaseDataFileStamp(GetProjectArtifactDatabasePath(normalizedProjectRoot));
    index.sources = std::move(sources->sources);
    index.sourceDirectories = std::move(sources->sourceDirectories);
    if (index.artifactDatabaseStamp.empty())
        return std::nullopt;

    for (const auto& source : index.sources)
    {
        const auto assetPath = ToStartupEditorAssetPath(source);
        if (!IsStartupPreimportAssetType(NLS::Core::Assets::InferAssetType(assetPath)))
            continue;
        if (database.AssetPathToGUID(assetPath).empty())
            continue;

        const auto manifest = database.GetArtifactManifestForAssetPath(assetPath);
        if (!manifest.has_value())
            return std::nullopt;
        AppendStartupDependencyEntries(index.dependencies, normalizedProjectRoot, assetPath, *manifest);

        for (const auto& artifact : manifest->subAssets)
        {
            const auto absoluteArtifactPath = database.ResolveArtifactPathAtPath(assetPath, artifact.subAssetKey);
            const auto metadata = FileMetadataForStartupCache(absoluteArtifactPath);
            const auto fastStamp = FastFileMetadataStampForStartupCache(absoluteArtifactPath);
            if (absoluteArtifactPath.empty() ||
                !metadata.has_value() ||
                fastStamp.empty() ||
                artifact.contentHash.empty())
            {
                return std::nullopt;
            }

            index.artifacts.push_back({
                NLS::Core::Assets::NormalizeAssetPath(absoluteArtifactPath).generic_string(),
                metadata->stamp,
                artifact.contentHash,
                fastStamp
            });
        }
    }

    SortStartupDependencyEntries(index.dependencies);
    index.dependencies.erase(
        std::unique(
            index.dependencies.begin(),
            index.dependencies.end(),
            [](const auto& lhs, const auto& rhs)
            {
                return lhs.ownerAssetPath == rhs.ownerAssetPath &&
                    lhs.relativePath == rhs.relativePath &&
                    lhs.stamp == rhs.stamp &&
                    lhs.contentHash == rhs.contentHash;
            }),
        index.dependencies.end());
    SortStartupArtifactEntries(index.artifacts);
    return index;
}

std::optional<StartupAssetPreimportSourceEntry> BuildStartupSourceEntryForEditorPath(
    const std::filesystem::path& projectRoot,
    const std::string& editorAssetPath,
    const std::unordered_map<std::string, StartupAssetPreimportSourceEntry>& previousByKey)
{
    const auto normalizedEditorPath = NormalizeEditorAssetPath(editorAssetPath);
    if (normalizedEditorPath.empty() || !IsStartupPreimportSourcePath(normalizedEditorPath))
        return std::nullopt;

    const auto roots = MakeStartupSourceRoots(projectRoot);
    for (const auto& root : roots)
    {
        const auto mountPath = NormalizeEditorAssetPath(root.mountPath);
        std::string relativePath;
        std::error_code error;
        if (std::filesystem::is_regular_file(root.path, error) && !error)
        {
            const auto rootEditorPath = NormalizeEditorAssetPath(
                std::filesystem::path(mountPath) / root.path.filename());
            if (rootEditorPath != normalizedEditorPath)
                continue;
            relativePath = NormalizeEditorAssetPath(root.path.filename());
        }
        else
        {
            const auto mountPrefix = mountPath + "/";
            if (normalizedEditorPath.rfind(mountPrefix, 0u) != 0u)
                continue;
            relativePath = NormalizeEditorAssetPath(normalizedEditorPath.substr(mountPrefix.size()));
        }

        const auto absolutePath = ResolveStartupRootMountedPath(
            roots,
            mountPath,
            relativePath);
        if (absolutePath.empty())
            continue;

        const auto metadata = FileMetadataForStartupCache(absolutePath);
        if (!metadata.has_value())
            continue;

        const auto previous = previousByKey.find(StartupSourceEntryKey(mountPath, relativePath));
        const auto contentHash = ResolveStartupContentHash(
            absolutePath,
            metadata->fingerprint,
            previous != previousByKey.end() ? previous->second.fingerprint : std::string {},
            previous != previousByKey.end() ? previous->second.contentHash : std::string {});
        if (contentHash.empty())
            continue;

        return StartupAssetPreimportSourceEntry {
            mountPath,
            relativePath,
            metadata->stamp,
            contentHash,
            metadata->fingerprint,
            absolutePath
        };
    }

    return std::nullopt;
}

bool TryPatchStartupAssetPreimportIndexForEmptyPlan(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& stampPath,
    const StartupAssetPreimportCacheAnalysis& cacheAnalysis)
{
    if (!cacheAnalysis.loadedIndex.has_value())
    {
        LogStartupIndexPatchSkipped("no-loaded-index");
        return false;
    }
    if (!cacheAnalysis.changedSourcePaths.has_value())
    {
        LogStartupIndexPatchSkipped("no-changed-source-list");
        return false;
    }
    if (cacheAnalysis.changedSourcePaths->empty())
    {
        LogStartupIndexPatchSkipped("empty-changed-source-list");
        return false;
    }
    if (cacheAnalysis.profile.missReason != "source-mismatch" &&
        cacheAnalysis.profile.missReason != "manifest-freshness-mismatch")
    {
        LogStartupIndexPatchSkipped("miss-reason-" + cacheAnalysis.profile.missReason);
        return false;
    }

    const auto normalizedProjectRoot = NLS::Core::Assets::NormalizeAssetPath(projectRoot);
    if (normalizedProjectRoot.empty())
    {
        LogStartupIndexPatchSkipped("empty-project-root");
        return false;
    }
    if (!StartupSourceDirectoriesMatchIndex(MakeStartupSourceRoots(normalizedProjectRoot), *cacheAnalysis.loadedIndex))
    {
        LogStartupIndexPatchSkipped("source-directory-mismatch");
        return false;
    }

    StartupAssetPreimportIndex patchedIndex = *cacheAnalysis.loadedIndex;
    patchedIndex.importerFingerprint = ComputeStartupImporterFingerprint();
    patchedIndex.artifactDatabaseStamp = GetArtifactDatabaseDataFileStamp(GetProjectArtifactDatabasePath(normalizedProjectRoot));
    if (patchedIndex.artifactDatabaseStamp.empty())
    {
        LogStartupIndexPatchSkipped("artifact-database-stamp-unavailable");
        return false;
    }
    std::unordered_map<std::string, size_t> sourceIndexByKey;
    sourceIndexByKey.reserve(patchedIndex.sources.size());
    std::unordered_map<std::string, StartupAssetPreimportSourceEntry> previousByKey;
    previousByKey.reserve(patchedIndex.sources.size());
    for (size_t index = 0u; index < patchedIndex.sources.size(); ++index)
    {
        const auto key = SourceEntryKey(patchedIndex.sources[index]);
        sourceIndexByKey.emplace(key, index);
        previousByKey.emplace(key, patchedIndex.sources[index]);
    }

    for (const auto& changedSourcePath : *cacheAnalysis.changedSourcePaths)
    {
        const auto currentSource = BuildStartupSourceEntryForEditorPath(
            normalizedProjectRoot,
            NormalizeEditorAssetPath(changedSourcePath),
            previousByKey);
        if (!currentSource.has_value())
        {
            LogStartupIndexPatchSkipped("changed-source-unavailable:" + NormalizeEditorAssetPath(changedSourcePath));
            return false;
        }

        const auto key = SourceEntryKey(*currentSource);
        const auto existing = sourceIndexByKey.find(key);
        if (existing == sourceIndexByKey.end())
        {
            LogStartupIndexPatchSkipped("changed-source-not-indexed:" + key);
            return false;
        }
        const auto& previousContentHash = patchedIndex.sources[existing->second].contentHash;
        if (!previousContentHash.empty() &&
            previousContentHash != currentSource->contentHash &&
            cacheAnalysis.profile.missReason != "manifest-freshness-mismatch")
        {
            LogStartupIndexPatchSkipped("changed-source-content-mismatch:" + key);
            return false;
        }
        if (currentSource->contentHash.empty())
        {
            LogStartupIndexPatchSkipped("changed-source-content-unavailable:" + key);
            return false;
        }
        patchedIndex.sources[existing->second] = *currentSource;
    }

    std::sort(
        patchedIndex.sources.begin(),
        patchedIndex.sources.end(),
        [](const auto& lhs, const auto& rhs)
        {
            return std::tie(lhs.rootMount, lhs.relativePath, lhs.stamp, lhs.contentHash) <
                std::tie(rhs.rootMount, rhs.relativePath, rhs.stamp, rhs.contentHash);
        });

    if (!WriteStartupAssetPreimportIndex(stampPath, patchedIndex))
    {
        LogStartupIndexPatchSkipped("write-failed");
        return false;
    }

    ++StartupAssetPreimportPatchedIndexWriteCountForTestingStorage();
    return true;
}

std::optional<StartupAssetPreimportIndex> LoadStartupAssetPreimportIndex(
    const std::filesystem::path& stampPath,
    const std::string* expectedProjectRoot = nullptr,
    const std::function<std::string()>& expectedImporterFingerprintProvider = {},
    const std::function<std::string()>& expectedArtifactDatabaseStampProvider = {})
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    ++StartupAssetPreimportIndexLoadCountForTestingStorage();
#endif
    std::ifstream input(stampPath, std::ios::binary);
    if (!input)
        return std::nullopt;

    StartupAssetPreimportIndex index;
    bool sawVersion = false;
    bool sawEnd = false;
    std::optional<size_t> expectedSourceCount;
    std::optional<size_t> expectedSourceDirectoryCount;
    std::optional<size_t> expectedDependencyCount;
    std::optional<size_t> expectedArtifactCount;
    std::string line;
    while (std::getline(input, line))
    {
        std::istringstream stream(line);
        std::string key;
        stream >> key;
        if (key == "version")
        {
            std::string version;
            stream >> std::quoted(version);
            if (version != kStartupAssetPreimportStampVersion)
                return std::nullopt;
            sawVersion = true;
        }
        else if (key == "projectRoot")
        {
            stream >> std::quoted(index.projectRoot);
            if (stream.fail())
                return std::nullopt;
            if (sawVersion &&
                expectedProjectRoot != nullptr &&
                index.projectRoot != *expectedProjectRoot)
            {
                return index;
            }
        }
        else if (key == "importers")
        {
            stream >> std::quoted(index.importerFingerprint);
            if (stream.fail())
                return std::nullopt;
            if (sawVersion &&
                expectedImporterFingerprintProvider &&
                (expectedProjectRoot == nullptr || index.projectRoot == *expectedProjectRoot) &&
                index.importerFingerprint != expectedImporterFingerprintProvider())
            {
                return index;
            }
        }
        else if (key == "artifactDb")
        {
            stream >> std::quoted(index.artifactDatabaseStamp);
            if (stream.fail())
                return std::nullopt;
            if (sawVersion &&
                expectedArtifactDatabaseStampProvider &&
                (expectedProjectRoot == nullptr || index.projectRoot == *expectedProjectRoot) &&
                (!expectedImporterFingerprintProvider || index.importerFingerprint == expectedImporterFingerprintProvider()) &&
                index.artifactDatabaseStamp != expectedArtifactDatabaseStampProvider())
            {
                return index;
            }
        }
        else if (key == "sourceCount")
        {
            size_t count = 0u;
            stream >> count;
            if (stream.fail())
                return std::nullopt;
            expectedSourceCount = count;
            index.sources.reserve(count);
        }
        else if (key == "dependencyCount")
        {
            size_t count = 0u;
            stream >> count;
            if (stream.fail())
                return std::nullopt;
            expectedDependencyCount = count;
            index.dependencies.reserve(count);
        }
        else if (key == "sourceDirectoryCount")
        {
            size_t count = 0u;
            stream >> count;
            if (stream.fail())
                return std::nullopt;
            expectedSourceDirectoryCount = count;
            index.sourceDirectories.reserve(count);
        }
        else if (key == "artifactCount")
        {
            size_t count = 0u;
            stream >> count;
            if (stream.fail())
                return std::nullopt;
            expectedArtifactCount = count;
            index.artifacts.reserve(count);
        }
        else if (key == "source")
        {
            StartupAssetPreimportSourceEntry source;
            stream >> std::quoted(source.rootMount)
                >> std::quoted(source.relativePath)
                >> std::quoted(source.stamp)
                >> std::quoted(source.contentHash)
                >> std::quoted(source.fingerprint);
            if (stream.fail())
                return std::nullopt;
            index.sources.push_back(std::move(source));
        }
        else if (key == "sourceDirectory")
        {
            StartupAssetPreimportDirectoryEntry directory;
            stream >> std::quoted(directory.rootMount)
                >> std::quoted(directory.relativePath)
                >> std::quoted(directory.stamp);
            if (stream.fail())
                return std::nullopt;
            index.sourceDirectories.push_back(std::move(directory));
        }
        else if (key == "dependency")
        {
            StartupAssetPreimportDependencyEntry dependency;
            stream >> std::quoted(dependency.ownerAssetPath)
                >> std::quoted(dependency.relativePath)
                >> std::quoted(dependency.stamp)
                >> std::quoted(dependency.contentHash)
                >> std::quoted(dependency.fingerprint);
            if (stream.fail())
                return std::nullopt;
            index.dependencies.push_back(std::move(dependency));
        }
        else if (key == "artifact")
        {
            StartupAssetPreimportArtifactEntry artifact;
            stream >> std::quoted(artifact.relativePath)
                >> std::quoted(artifact.stamp)
                >> std::quoted(artifact.contentHash)
                >> std::quoted(artifact.fingerprint);
            if (stream.fail())
                return std::nullopt;
            index.artifacts.push_back(std::move(artifact));
        }
        else if (key == "end")
        {
            std::string version;
            stream >> std::quoted(version);
            if (version != kStartupAssetPreimportStampVersion)
                return std::nullopt;
            sawEnd = true;
        }
    }

    if (!sawVersion ||
        !sawEnd ||
        index.projectRoot.empty() ||
        index.importerFingerprint.empty() ||
        index.artifactDatabaseStamp.empty())
    {
        return std::nullopt;
    }
    if (!expectedSourceCount.has_value() ||
        !expectedSourceDirectoryCount.has_value() ||
        !expectedDependencyCount.has_value() ||
        !expectedArtifactCount.has_value() ||
        *expectedSourceCount != index.sources.size() ||
        *expectedSourceDirectoryCount != index.sourceDirectories.size() ||
        *expectedDependencyCount != index.dependencies.size() ||
        *expectedArtifactCount != index.artifacts.size())
    {
        return std::nullopt;
    }
    std::sort(
        index.sourceDirectories.begin(),
        index.sourceDirectories.end(),
        [](const auto& lhs, const auto& rhs)
        {
            return std::tie(lhs.rootMount, lhs.relativePath, lhs.stamp) <
                std::tie(rhs.rootMount, rhs.relativePath, rhs.stamp);
        });
    SortStartupDependencyEntries(index.dependencies);
    SortStartupArtifactEntries(index.artifacts);
    return index;
}

bool WriteStartupAssetPreimportIndex(
    const std::filesystem::path& stampPath,
    const StartupAssetPreimportIndex& index)
{
    std::error_code error;
    std::filesystem::create_directories(stampPath.parent_path(), error);
    if (error)
        return false;

    auto tempPath = stampPath;
    tempPath += ".tmp";
    std::ostringstream serialized;
    serialized << "version " << std::quoted(std::string(kStartupAssetPreimportStampVersion)) << '\n';
    serialized << "projectRoot " << std::quoted(index.projectRoot) << '\n';
    serialized << "importers " << std::quoted(index.importerFingerprint) << '\n';
    serialized << "artifactDb " << std::quoted(index.artifactDatabaseStamp) << '\n';
    serialized << "sourceCount " << index.sources.size() << '\n';
    serialized << "sourceDirectoryCount " << index.sourceDirectories.size() << '\n';
    serialized << "dependencyCount " << index.dependencies.size() << '\n';
    serialized << "artifactCount " << index.artifacts.size() << '\n';
    for (const auto& source : index.sources)
    {
        serialized << "source "
            << std::quoted(source.rootMount) << ' '
            << std::quoted(source.relativePath) << ' '
            << std::quoted(source.stamp) << ' '
            << std::quoted(source.contentHash) << ' '
            << std::quoted(source.fingerprint) << '\n';
    }
    for (const auto& directory : index.sourceDirectories)
    {
        serialized << "sourceDirectory "
            << std::quoted(directory.rootMount) << ' '
            << std::quoted(directory.relativePath) << ' '
            << std::quoted(directory.stamp) << '\n';
    }
    for (const auto& dependency : index.dependencies)
    {
        serialized << "dependency "
            << std::quoted(dependency.ownerAssetPath) << ' '
            << std::quoted(dependency.relativePath) << ' '
            << std::quoted(dependency.stamp) << ' '
            << std::quoted(dependency.contentHash) << ' '
            << std::quoted(dependency.fingerprint) << '\n';
    }
    for (const auto& artifact : index.artifacts)
    {
        serialized << "artifact "
            << std::quoted(artifact.relativePath) << ' '
            << std::quoted(artifact.stamp) << ' '
            << std::quoted(artifact.contentHash) << ' '
            << std::quoted(artifact.fingerprint) << '\n';
    }
    serialized << "end " << std::quoted(std::string(kStartupAssetPreimportStampVersion)) << '\n';
    const auto serializedText = serialized.str();

    {
        std::ifstream existing(stampPath, std::ios::binary);
        if (existing)
        {
            const std::string existingText {
                std::istreambuf_iterator<char>(existing),
                std::istreambuf_iterator<char>()};
            if (existingText == serializedText)
                return true;
        }
    }

    std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    output << serializedText;
    output.close();
    if (!output)
    {
        std::filesystem::remove(tempPath, error);
        return false;
    }

    error.clear();
    std::filesystem::rename(tempPath, stampPath, error);
    if (error)
    {
        error.clear();
        std::filesystem::remove(stampPath, error);
        error.clear();
        std::filesystem::rename(tempPath, stampPath, error);
    }
    if (error)
    {
        std::filesystem::remove(tempPath, error);
        return false;
    }
    ++StartupAssetPreimportShardWriteCountForTestingStorage();
    return true;
}

StartupAssetPreimportCacheAnalysis AnalyzeStartupAssetPreimportCache(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& stampPath)
{
    StartupAssetPreimportCacheAnalysis analysis;
    const auto checkBegin = std::chrono::steady_clock::now();
    const auto metadataQueriesAtBegin = StartupAssetPreimportFileMetadataQueryCountForTestingStorage();
    const auto contentHashReadsAtBegin = StartupAssetPreimportContentHashReadCountForTestingStorage();
    const auto importerFingerprintComputesAtBegin =
        StartupAssetPreimportImporterFingerprintComputeCountForTestingStorage();
    const auto finishAnalysis =
        [
            &analysis,
            checkBegin,
            metadataQueriesAtBegin,
            contentHashReadsAtBegin,
            importerFingerprintComputesAtBegin
        ]() -> StartupAssetPreimportCacheAnalysis
        {
            analysis.profile.trackedFileEntryCount =
                analysis.profile.sourceEntryCount +
                analysis.profile.dependencyEntryCount +
                analysis.profile.artifactEntryCount;
            analysis.profile.elapsedMilliseconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - checkBegin).count());
            analysis.profile.fileMetadataQueryCount =
                StartupAssetPreimportFileMetadataQueryCountForTestingStorage() - metadataQueriesAtBegin;
            analysis.profile.contentHashReadCount =
                StartupAssetPreimportContentHashReadCountForTestingStorage() - contentHashReadsAtBegin;
            analysis.profile.importerFingerprintComputeCount =
                StartupAssetPreimportImporterFingerprintComputeCountForTestingStorage() -
                    importerFingerprintComputesAtBegin;
            return analysis;
        };
    const auto finishMiss =
        [&analysis, &finishAnalysis](std::string reason) -> StartupAssetPreimportCacheAnalysis
        {
            analysis.profile.missReason = std::move(reason);
            return finishAnalysis();
    };
    const auto normalizedProjectRoot = NLS::Core::Assets::NormalizeAssetPath(projectRoot);
    const auto normalizedProjectRootString = normalizedProjectRoot.generic_string();
    std::string importerFingerprint;
    bool importerFingerprintResolved = false;
    const auto resolveImporterFingerprint =
        [&importerFingerprint, &importerFingerprintResolved]() -> std::string
        {
            if (!importerFingerprintResolved)
            {
                importerFingerprint = ComputeStartupImporterFingerprint();
                importerFingerprintResolved = true;
            }
            return importerFingerprint;
        };
    std::string artifactDatabaseStamp;
    bool artifactDatabaseStampResolved = false;
    const auto resolveArtifactDatabaseStamp =
        [&artifactDatabaseStamp, &artifactDatabaseStampResolved, &normalizedProjectRoot]() -> std::string
        {
            if (!artifactDatabaseStampResolved)
            {
                artifactDatabaseStamp = GetArtifactDatabaseDataFileStamp(GetProjectArtifactDatabasePath(normalizedProjectRoot));
                artifactDatabaseStampResolved = true;
            }
            return artifactDatabaseStamp;
        };
    auto validationStageBegin = std::chrono::steady_clock::now();
    const auto logValidationStage =
        [&validationStageBegin](std::string_view stage, const size_t itemCount = 0u)
        {
            LogStartupTiming(stage, validationStageBegin, itemCount);
            validationStageBegin = std::chrono::steady_clock::now();
        };
    auto index = LoadStartupAssetPreimportIndex(
        stampPath,
        &normalizedProjectRootString,
        resolveImporterFingerprint,
        resolveArtifactDatabaseStamp);
    logValidationStage("Cache validation stage LoadIndex");
    if (!index.has_value())
    {
        return finishMiss("index-unavailable");
    }
    analysis.loadedIndex = *index;
    if (index->projectRoot != normalizedProjectRootString)
    {
        return finishMiss("project-root-mismatch");
    }
    const auto currentImporterFingerprint = resolveImporterFingerprint();
    const bool manifestFreshnessMismatch = index->importerFingerprint != currentImporterFingerprint;
    std::optional<std::vector<NLS::Core::Assets::AssetType>> manifestFreshnessAffectedTypes;
    if (manifestFreshnessMismatch)
    {
        if (auto affectedTypes = TryGetStartupManifestFreshnessAffectedAssetTypes(
                index->importerFingerprint,
                currentImporterFingerprint);
            affectedTypes.has_value())
        {
            manifestFreshnessAffectedTypes = std::move(*affectedTypes);
        }

        if (index->sources.empty() &&
            index->sourceDirectories.empty() &&
            index->dependencies.empty() &&
            index->artifacts.empty())
        {
            auto fullIndex = LoadStartupAssetPreimportIndex(stampPath);
            logValidationStage("Cache validation stage LoadFullIndexForImporterMismatch");
            if (fullIndex.has_value())
            {
                analysis.loadedIndex = *fullIndex;
                index = fullIndex;
            }
        }
    }
    const auto currentArtifactDatabaseStamp = resolveArtifactDatabaseStamp();
    if (currentArtifactDatabaseStamp.empty())
        return finishMiss("artifact-database-unavailable");
    if (manifestFreshnessMismatch && index->artifactDatabaseStamp.empty())
        return finishMiss("manifest-freshness-mismatch");
    const bool artifactDatabaseStampMismatch = index->artifactDatabaseStamp != currentArtifactDatabaseStamp;
    if (artifactDatabaseStampMismatch)
        return finishMiss("artifact-database-stamp-mismatch");
    analysis.profile.sourceEntryCount = index->sources.size();
    analysis.profile.sourceDirectoryEntryCount = index->sourceDirectories.size();
    analysis.profile.dependencyEntryCount = index->dependencies.size();
    analysis.profile.artifactEntryCount = index->artifacts.size();

    std::unordered_map<std::string, StartupAssetPreimportSourceEntry> previousSourcesByKey;
    previousSourcesByKey.reserve(index->sources.size());
    for (const auto& source : index->sources)
        previousSourcesByKey.emplace(SourceEntryKey(source), source);
    logValidationStage("Cache validation stage BuildPreviousSourceMap", index->sources.size());

    auto currentSourceCollection = TryCollectStartupSourceEntriesFromIndex(
        normalizedProjectRoot,
        *index,
        previousSourcesByKey);
    logValidationStage("Cache validation stage CollectSourcesFromIndex", index->sources.size());
    if (!currentSourceCollection.has_value())
    {
        currentSourceCollection = CollectStartupSourceEntries(normalizedProjectRoot, &previousSourcesByKey);
        logValidationStage("Cache validation stage CollectSourcesByEnumeration");
    }
    if (!currentSourceCollection.has_value())
    {
        return finishMiss("source-collection-failed");
    }
    const auto& currentSources = currentSourceCollection->sources;
    const auto currentDependencies = CollectStartupDependencyEntriesFromIndex(normalizedProjectRoot, *index);
    logValidationStage("Cache validation stage CollectDependenciesFromIndex", index->dependencies.size());
    if (!DependencyEntriesEqual(index->dependencies, currentDependencies))
    {
        return finishMiss("dependency-mismatch");
    }

    bool needsFastArtifactStampPatch = false;
    StartupAssetPreimportIndex patchedFastArtifactIndex = *index;
    for (size_t artifactIndex = 0u; artifactIndex < index->artifacts.size(); ++artifactIndex)
    {
        const auto& artifact = index->artifacts[artifactIndex];
        const auto absolutePath = ResolveStartupArtifactPath(normalizedProjectRoot, artifact.relativePath);
        if (artifact.fingerprint.rfind("fast:", 0u) == 0u)
        {
            const auto stampMatches = FastFileMetadataStampMatchesStartupCache(absolutePath, artifact.fingerprint);
            if (!stampMatches.has_value())
            {
                return finishMiss("artifact-metadata-unavailable");
            }
            if (!stampMatches.value())
            {
                const auto currentStamp = FileStampForStartupCache(absolutePath);
                const auto fastStamp = FastFileMetadataStampForStartupCache(absolutePath);
                if (currentStamp.empty() || fastStamp.empty())
                    return finishMiss("artifact-metadata-unavailable");
                if (!ArtifactPayloadContentMatchesStartupCache(absolutePath, artifact.contentHash))
                    return finishMiss("artifact-content-mismatch");
                patchedFastArtifactIndex.artifacts[artifactIndex].stamp = currentStamp;
                patchedFastArtifactIndex.artifacts[artifactIndex].fingerprint = fastStamp;
                needsFastArtifactStampPatch = true;
            }
            continue;
        }

        const auto currentStamp = FileStampForStartupCache(absolutePath);
        if (currentStamp.empty())
        {
            return finishMiss("artifact-metadata-unavailable");
        }
        const auto fastStamp = FastFileMetadataStampForStartupCache(absolutePath);
        if (fastStamp.empty())
            return finishMiss("artifact-metadata-unavailable");
        if (currentStamp != artifact.stamp)
        {
            if (!ArtifactPayloadContentMatchesStartupCache(absolutePath, artifact.contentHash))
                return finishMiss("artifact-content-mismatch");
            patchedFastArtifactIndex.artifacts[artifactIndex].stamp = currentStamp;
            patchedFastArtifactIndex.artifacts[artifactIndex].fingerprint = fastStamp;
            needsFastArtifactStampPatch = true;
            continue;
        }
        patchedFastArtifactIndex.artifacts[artifactIndex].fingerprint = fastStamp;
        needsFastArtifactStampPatch = true;
    }
    logValidationStage(
        needsFastArtifactStampPatch
            ? "Cache validation stage ValidateArtifactLegacyStamps"
            : "Cache validation stage ValidateArtifactFastStamps",
        index->artifacts.size());
    if (needsFastArtifactStampPatch)
    {
        analysis.loadedIndex = std::move(patchedFastArtifactIndex);
        analysis.patchLoadedIndexOnCacheHit = true;
    }

    analysis.currentSourceAssetPaths.reserve(currentSources.size());
    for (const auto& source : currentSources)
    {
        if (!source.absolutePath.empty() && !NLS::Core::Assets::IsMetaFilePath(source.absolutePath))
            analysis.currentSourceAssetPaths.push_back(source.absolutePath);
    }
    logValidationStage("Cache validation stage BuildCurrentSourcePaths", currentSources.size());

    std::vector<std::filesystem::path> changedPaths;
    std::vector<std::filesystem::path> manifestAffectedEditorPaths;
    std::vector<std::filesystem::path> sourceChangedEditorPaths;
    std::vector<std::filesystem::path> manifestAffectedAbsoluteSourcePaths;
    std::vector<std::filesystem::path> sourceChangedAbsoluteSourcePaths;
    if (manifestFreshnessMismatch &&
        manifestFreshnessAffectedTypes.has_value() &&
        !manifestFreshnessAffectedTypes->empty())
    {
        for (const auto& source : currentSources)
        {
            const auto editorAssetPath = ToStartupEditorAssetPath(source);
            if (IsStartupManifestFreshnessAffectedSourcePath(editorAssetPath, *manifestFreshnessAffectedTypes))
            {
                changedPaths.push_back(editorAssetPath);
                manifestAffectedEditorPaths.push_back(editorAssetPath);
                if (!source.absolutePath.empty() && !NLS::Core::Assets::IsMetaFilePath(source.absolutePath))
                    manifestAffectedAbsoluteSourcePaths.push_back(source.absolutePath);
            }
        }
    }

    const bool sourceEntriesChanged = !SourceEntriesEqual(index->sources, currentSources);
    if (sourceEntriesChanged)
    {
        for (const auto& source : currentSources)
        {
            const auto key = SourceEntryKey(source);
            const auto previous = previousSourcesByKey.find(key);
            if (previous == previousSourcesByKey.end())
            {
                const auto editorAssetPath = ToStartupEditorAssetPath(source);
                changedPaths.push_back(editorAssetPath);
                sourceChangedEditorPaths.push_back(editorAssetPath);
                if (!source.absolutePath.empty() && !NLS::Core::Assets::IsMetaFilePath(source.absolutePath))
                    sourceChangedAbsoluteSourcePaths.push_back(source.absolutePath);
                continue;
            }

            if (!SourceEntryMetadataEqual(source, previous->second))
            {
                const auto editorAssetPath = ToStartupEditorAssetPath(source);
                changedPaths.push_back(editorAssetPath);
                sourceChangedEditorPaths.push_back(editorAssetPath);
                if (!source.absolutePath.empty() && !NLS::Core::Assets::IsMetaFilePath(source.absolutePath))
                    sourceChangedAbsoluteSourcePaths.push_back(source.absolutePath);
            }
            previousSourcesByKey.erase(previous);
        }

        for (const auto& [_, removedSource] : previousSourcesByKey)
        {
            const auto editorAssetPath = ToStartupEditorAssetPath(removedSource);
            changedPaths.push_back(editorAssetPath);
            sourceChangedEditorPaths.push_back(editorAssetPath);
            if (!removedSource.absolutePath.empty() && !NLS::Core::Assets::IsMetaFilePath(removedSource.absolutePath))
                sourceChangedAbsoluteSourcePaths.push_back(removedSource.absolutePath);
        }
    }
    logValidationStage("Cache validation stage DetectChangedSources", currentSources.size());

    if (!changedPaths.empty())
    {
        std::sort(changedPaths.begin(), changedPaths.end());
        changedPaths.erase(std::unique(changedPaths.begin(), changedPaths.end()), changedPaths.end());
        LogStartupTiming(
            manifestFreshnessMismatch
                ? "Manifest-freshness startup cache check"
                : "Changed-source startup cache check",
            checkBegin,
            changedPaths.size());
        auto manifestCandidateAssetPaths = BuildStartupPreimportCandidateAssetPaths(
            *index,
            manifestAffectedEditorPaths,
            false);
        auto sourceCandidateAssetPaths = BuildStartupPreimportCandidateAssetPaths(
            *index,
            sourceChangedEditorPaths,
            false);
        analysis.candidatePreimportAssetPaths = std::move(manifestCandidateAssetPaths);
        analysis.candidatePreimportAssetPaths.insert(
            analysis.candidatePreimportAssetPaths.end(),
            sourceCandidateAssetPaths.begin(),
            sourceCandidateAssetPaths.end());
        std::sort(analysis.candidatePreimportAssetPaths.begin(), analysis.candidatePreimportAssetPaths.end());
        analysis.candidatePreimportAssetPaths.erase(
            std::unique(
                analysis.candidatePreimportAssetPaths.begin(),
                analysis.candidatePreimportAssetPaths.end()),
            analysis.candidatePreimportAssetPaths.end());
        size_t resolvedRefreshSourcePathCount = 0u;
        const size_t sourceChangedAbsoluteSourcePathCount = sourceChangedAbsoluteSourcePaths.size();
        const size_t manifestAffectedAbsoluteSourcePathCount = manifestAffectedAbsoluteSourcePaths.size();
        if (sourceEntriesChanged)
        {
            std::sort(sourceChangedAbsoluteSourcePaths.begin(), sourceChangedAbsoluteSourcePaths.end());
            sourceChangedAbsoluteSourcePaths.erase(
                std::unique(sourceChangedAbsoluteSourcePaths.begin(), sourceChangedAbsoluteSourcePaths.end()),
                sourceChangedAbsoluteSourcePaths.end());
            if (sourceChangedAbsoluteSourcePaths.empty())
                sourceChangedAbsoluteSourcePaths = ResolveStartupChangedSourceAssetPaths(normalizedProjectRoot, changedPaths);
            analysis.targetedRefreshSourceAssetPaths = sourceChangedAbsoluteSourcePaths.empty()
                ? analysis.currentSourceAssetPaths
                : std::move(sourceChangedAbsoluteSourcePaths);
        }
        else
        {
            std::sort(manifestAffectedAbsoluteSourcePaths.begin(), manifestAffectedAbsoluteSourcePaths.end());
            manifestAffectedAbsoluteSourcePaths.erase(
                std::unique(manifestAffectedAbsoluteSourcePaths.begin(), manifestAffectedAbsoluteSourcePaths.end()),
                manifestAffectedAbsoluteSourcePaths.end());
            analysis.targetedRefreshSourceAssetPaths = std::move(manifestAffectedAbsoluteSourcePaths);
        }
        std::vector<std::filesystem::path> refreshEditorPaths;
        refreshEditorPaths.reserve(analysis.candidatePreimportAssetPaths.size() + 1u);
        for (const auto& candidateAssetPath : analysis.candidatePreimportAssetPaths)
            refreshEditorPaths.push_back(candidateAssetPath);
        refreshEditorPaths.push_back(kProjectStandardPbrShaderPath);
        auto requiredRefreshSourcePaths = ResolveStartupChangedSourceAssetPaths(normalizedProjectRoot, refreshEditorPaths);
        analysis.targetedRefreshSourceAssetPaths.insert(
            analysis.targetedRefreshSourceAssetPaths.end(),
            requiredRefreshSourcePaths.begin(),
            requiredRefreshSourcePaths.end());
        std::sort(analysis.targetedRefreshSourceAssetPaths.begin(), analysis.targetedRefreshSourceAssetPaths.end());
        analysis.targetedRefreshSourceAssetPaths.erase(
            std::unique(
                analysis.targetedRefreshSourceAssetPaths.begin(),
                analysis.targetedRefreshSourceAssetPaths.end()),
            analysis.targetedRefreshSourceAssetPaths.end());
        resolvedRefreshSourcePathCount = analysis.targetedRefreshSourceAssetPaths.size();
        logValidationStage(
            "Cache validation stage BuildTargetedRefreshPaths",
            analysis.targetedRefreshSourceAssetPaths.size());
        analysis.changedSourcePaths = std::move(changedPaths);
        NLS_LOG_INFO(
            "[StartupAssetPreimport] Targeted startup miss paths: changed=" +
            std::to_string(analysis.changedSourcePaths->size()) +
            " refresh=" + std::to_string(resolvedRefreshSourcePathCount) +
            " candidates=" + std::to_string(analysis.candidatePreimportAssetPaths.size()) +
            " sourceEntriesChanged=" + std::string(sourceEntriesChanged ? "true" : "false") +
            " sourceChangedAbs=" + std::to_string(sourceChangedAbsoluteSourcePathCount) +
            " manifestAffectedAbs=" + std::to_string(manifestAffectedAbsoluteSourcePathCount));
        if (!analysis.candidatePreimportAssetPaths.empty())
        {
            std::ostringstream candidates;
            for (size_t index = 0u; index < analysis.candidatePreimportAssetPaths.size(); ++index)
            {
                if (index != 0u)
                    candidates << ", ";
                candidates << analysis.candidatePreimportAssetPaths[index];
            }
            NLS_LOG_INFO("[StartupAssetPreimport] Targeted startup candidate assets: " + candidates.str());
        }
        analysis.profile.missReason = manifestFreshnessMismatch
            ? "manifest-freshness-mismatch"
            : "source-mismatch";
        return finishAnalysis();
    }

    if (manifestFreshnessMismatch)
        return finishMiss("manifest-freshness-mismatch");

    LogStartupTiming(
        "Cache validation",
        checkBegin,
        index->sources.size() + index->dependencies.size() + index->artifacts.size());
    analysis.cacheHit = true;
    return finishAnalysis();
}

void LogStartupAssetPreimportProgress(const ImportProgressEvent& event)
{
    NLS_LOG_INFO("[StartupAssetPreimport] " + FormatStartupAssetPreimportProgressLabel(event));
}

void PublishStartupAssetPreimportProgress(
    const StartupAssetPreimportProgressSink& progressSink,
    const ImportPhase phase,
    const double normalizedProgress,
    std::string message,
    std::string sourcePath)
{
    ImportProgressEvent event;
    event.sourcePath = std::move(sourcePath);
    event.targetPlatform = "editor";
    event.phase = phase;
    event.normalizedProgress = normalizedProgress;
    event.message = std::move(message);
    LogStartupAssetPreimportProgress(event);
    if (progressSink)
        progressSink(event);
}

PreparedPrefabCachePreflightSummary PreflightPreparedPrefabCache(
    const std::filesystem::path& projectRoot,
    const AssetDatabaseFacade& database,
    const size_t maxPreflightCount,
    const std::chrono::milliseconds maxPreflightDuration,
    const std::vector<std::string>& priorityAssetPaths,
    const StartupAssetPreimportProgressSink& progressSink)
{
    PreparedPrefabCachePreflightSummary summary;
    if (maxPreflightCount == 0u)
        return summary;

    std::error_code error;
    if (!std::filesystem::is_directory(projectRoot / "Library" / "PreparedPrefabCache", error) || error)
        return summary;

    auto modelAssets = database.FindAssets("type:model-scene", {});
    for (auto& assetPath : modelAssets)
        assetPath = NormalizeEditorAssetPath(assetPath);
    modelAssets.erase(
        std::remove_if(
            modelAssets.begin(),
            modelAssets.end(),
            [](const std::string& assetPath)
            {
                return assetPath.rfind("Assets/", 0u) != 0u;
            }),
        modelAssets.end());
    std::unordered_map<std::string, size_t> priorityByAssetPath;
    for (size_t index = 0u; index < priorityAssetPaths.size(); ++index)
    {
        const auto normalizedPath = NormalizeEditorAssetPath(priorityAssetPaths[index]);
        if (!normalizedPath.empty() &&
            normalizedPath.rfind("Assets/", 0u) == 0u)
        {
            priorityByAssetPath.emplace(normalizedPath, index);
        }
    }
    std::sort(
        modelAssets.begin(),
        modelAssets.end(),
        [&priorityByAssetPath](const std::string& lhs, const std::string& rhs)
        {
            const auto lhsPriority = priorityByAssetPath.find(lhs);
            const auto rhsPriority = priorityByAssetPath.find(rhs);
            const bool lhsHasPriority = lhsPriority != priorityByAssetPath.end();
            const bool rhsHasPriority = rhsPriority != priorityByAssetPath.end();
            if (lhsHasPriority != rhsHasPriority)
                return lhsHasPriority;
            if (lhsHasPriority && rhsHasPriority && lhsPriority->second != rhsPriority->second)
                return lhsPriority->second < rhsPriority->second;
            return lhs < rhs;
        });
    modelAssets.erase(std::unique(modelAssets.begin(), modelAssets.end()), modelAssets.end());
    if (modelAssets.empty())
        return summary;

    PublishStartupAssetPreimportProgress(
        progressSink,
        ImportPhase::Queued,
        0.96,
        "Preparing imported prefab cache",
        std::to_string(std::min(maxPreflightCount, modelAssets.size())) + " model assets");

    EditorAssetDragDropBridge bridge(projectRoot / "Assets");
    const auto preflightBegin = std::chrono::steady_clock::now();
    for (const auto& assetPath : modelAssets)
    {
        if (summary.attemptedCount >= maxPreflightCount)
            break;
        if (summary.attemptedCount > 0u &&
            maxPreflightDuration.count() >= 0 &&
            std::chrono::steady_clock::now() - preflightBegin >= maxPreflightDuration)
        {
            break;
        }
        ++summary.attemptedCount;

        const auto guid = database.AssetPathToGUID(assetPath);
        auto assetId = guid.empty()
            ? NLS::Core::Assets::AssetId {}
            : NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));
        if (!assetId.IsValid())
            continue;

        UnifiedPrefabLoadRequest request;
        request.source = NormalizePrefabSourceIdentity(
            projectRoot,
            assetPath,
            "prefab:" + std::filesystem::path(assetPath).stem().generic_string(),
            assetId,
            NLS::Core::Assets::AssetType::ModelScene);
        request.loadMode = UnifiedPrefabLoadMode::Prewarm;
        request.ownerKind = UnifiedPrefabOwnerKind::AsyncJob;
        request.ownerScopeId = "startup-prepared-prefab-cache";
        request.requiredReadiness = UnifiedPrefabReadiness::PrefabGraphOnly;
        request.allowPending = true;

        if (bridge.TryPreloadExistingPreparedPrefabHotCache(request))
            ++summary.preparedCount;
    }
    return summary;
}

}

std::string FormatStartupAssetPreimportProgressLabel(const ImportProgressEvent& event)
{
    if (event.message.empty())
        return event.sourcePath;
    if (event.sourcePath.empty())
        return event.message;

    auto fileName = std::filesystem::path(event.sourcePath).filename().generic_string();
    if (fileName.empty())
        fileName = event.sourcePath;
    if (fileName == event.sourcePath)
        return event.message + ": " + event.sourcePath;
    return event.message + ": " + fileName + " (" + event.sourcePath + ")";
}

StartupAssetPreimportResult RunBlockingStartupAssetPreimport(
    const StartupAssetPreimportOptions& options,
    StartupAssetPreimportProgressSink progressSink)
{
    StartupAssetPreimportResult result;
    if (options.projectRoot.empty())
        return result;

    const auto startupStampPath = GetStartupAssetPreimportStampPath(options.projectRoot);
    PublishStartupAssetPreimportProgress(
        progressSink,
        ImportPhase::Queued,
        0.001,
        "Checking startup asset cache",
        "Assets");
    const auto cacheCheckBegin = std::chrono::steady_clock::now();
    const auto cacheAnalysis = AnalyzeStartupAssetPreimportCache(options.projectRoot, startupStampPath);
    result.cacheValidationProfile = cacheAnalysis.profile;
    LogStartupCacheValidationProfile(result.cacheValidationProfile);
    if (cacheAnalysis.cacheHit)
    {
        if (cacheAnalysis.patchLoadedIndexOnCacheHit && cacheAnalysis.loadedIndex.has_value())
        {
            const auto indexPatchBegin = std::chrono::steady_clock::now();
            if (WriteStartupAssetPreimportIndex(startupStampPath, *cacheAnalysis.loadedIndex))
            {
                ++StartupAssetPreimportPatchedIndexWriteCountForTestingStorage();
                LogStartupTiming("Startup asset cache index patch", indexPatchBegin);
            }
            else
            {
                NLS_LOG_WARNING("[StartupAssetPreimport] Failed to patch startup asset preimport cache stamp.");
            }
        }
        LogStartupTiming("Startup asset cache hit", cacheCheckBegin);
        result.succeeded = true;
        result.usedCache = true;
        PublishStartupAssetPreimportProgress(
            progressSink,
            ImportPhase::Finished,
            1.0,
            "Startup asset artifacts are current",
            "Assets");
        return result;
    }
    LogStartupTiming("Startup asset cache miss analysis", cacheCheckBegin);

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(options.projectRoot));
    ImportProgressTracker tracker;
    tracker.Subscribe([progressSink](const ImportProgressEvent& event)
    {
        LogStartupAssetPreimportProgress(event);
        if (progressSink)
            progressSink(event);
    });

    AssetPreimportScheduler scheduler;
    PublishStartupAssetPreimportProgress(
        progressSink,
        ImportPhase::Queued,
        0.01,
        "Scanning project assets",
        "Assets");
    const auto refreshBegin = std::chrono::steady_clock::now();
    const auto& targetedRefreshSourcePaths = cacheAnalysis.targetedRefreshSourceAssetPaths.empty()
        ? cacheAnalysis.currentSourceAssetPaths
        : cacheAnalysis.targetedRefreshSourceAssetPaths;
    const bool canUseTargetedRefresh =
        cacheAnalysis.changedSourcePaths.has_value() &&
        !cacheAnalysis.changedSourcePaths->empty() &&
        !targetedRefreshSourcePaths.empty();
    bool refreshSucceeded = canUseTargetedRefresh
        ? database.RefreshKnownSourceAssets(targetedRefreshSourcePaths)
        : database.Refresh();
    if (!refreshSucceeded && canUseTargetedRefresh)
        refreshSucceeded = database.Refresh();
    if (!refreshSucceeded)
    {
        result.diagnostics = database.GetDiagnostics();
        result.hadRunningJobsAfterCompletion = tracker.HasRunningJobs();
        return result;
    }
    LogStartupTiming(
        canUseTargetedRefresh
            ? "Startup asset targeted database refresh"
            : "Startup asset database refresh",
        refreshBegin);

    if (database.AssetPathToGUID(kProjectStandardPbrShaderPath).empty())
    {
        NLS::Core::Assets::AssetDiagnostic diagnostic;
        diagnostic.severity = NLS::Core::Assets::AssetDiagnosticSeverity::Error;
        diagnostic.code = "startup-standard-pbr-source-missing";
        diagnostic.path = kProjectStandardPbrShaderPath;
        diagnostic.message =
            "Startup asset preimport could not find the built-in StandardPBR ShaderLab source in the mounted engine shader root.";
        result.diagnostics.push_back(std::move(diagnostic));
        result.hadRunningJobsAfterCompletion = tracker.HasRunningJobs();
        return result;
    }

    size_t internalImportCount = 0u;
    bool standardPbrDependencyReady = database.IsArtifactManifestCurrentForAssetPath(kProjectStandardPbrShaderPath);
    if (!standardPbrDependencyReady)
    {
        PublishStartupAssetPreimportProgress(
            progressSink,
            ImportPhase::Queued,
            0.025,
            "Importing built-in shader dependency",
            kProjectStandardPbrShaderPath);
        const auto beforeInternalImportCount = database.GetCompletedImportCount();
        if (!database.ImportAsset(kProjectStandardPbrShaderPath))
        {
            NLS_LOG_WARNING(
                "[StartupAssetPreimport] Built-in StandardPBR ShaderLab artifact import failed; "
                "model material imports will keep referencing the ShaderLab source and shader variants can be rebuilt later.");
        }
        else
        {
            internalImportCount = database.GetCompletedImportCount() - beforeInternalImportCount;
            standardPbrDependencyReady = true;
            const auto normalizedProjectRoot =
                NLS::Core::Assets::NormalizeAssetPath(options.projectRoot);
            const auto standardPbrRefreshPaths =
                ResolveStartupChangedSourceAssetPaths(normalizedProjectRoot, {kProjectStandardPbrShaderPath});
            const bool refreshedStandardPbrSource = !standardPbrRefreshPaths.empty() &&
                database.RefreshKnownSourceAssets(standardPbrRefreshPaths);
            if (!refreshedStandardPbrSource)
            {
                result.diagnostics = database.GetDiagnostics();
                result.hadRunningJobsAfterCompletion = tracker.HasRunningJobs();
                return result;
            }
        }
    }

    PublishStartupAssetPreimportProgress(
        progressSink,
        ImportPhase::Queued,
        0.03,
        "Planning startup asset imports",
        "Assets");
    const auto planBegin = std::chrono::steady_clock::now();
    const auto plan = cacheAnalysis.changedSourcePaths.has_value() && !cacheAnalysis.changedSourcePaths->empty()
        ? scheduler.BuildPlan(
            database,
            {
                AssetPreimportReason::EditorStartup,
                *cacheAnalysis.changedSourcePaths,
                cacheAnalysis.candidatePreimportAssetPaths
            })
        : scheduler.BuildPlan(database, AssetPreimportReason::EditorStartup);
    result.plannedAssetCount = plan.assetPaths.size();
    LogStartupTiming("Startup asset import planning", planBegin, result.plannedAssetCount);
    PublishStartupAssetPreimportProgress(
        progressSink,
        ImportPhase::Queued,
        plan.assetPaths.empty() ? 1.0 : 0.04,
        plan.assetPaths.empty()
            ? "Startup asset artifacts are current"
            : "Importing startup assets",
        plan.assetPaths.empty()
            ? "Assets"
            : std::to_string(plan.assetPaths.size()) + " assets");

    const auto importBegin = std::chrono::steady_clock::now();
    result.succeeded = scheduler.RunAlreadyPlanned(
        database,
        tracker,
        {AssetPreimportReason::EditorStartup, {}},
        plan);
    result.importedAssetCount =
        database.GetCompletedImportCount() >= internalImportCount
            ? database.GetCompletedImportCount() - internalImportCount
            : 0u;
    LogStartupTiming("Startup asset import execution", importBegin, result.importedAssetCount);
    if (result.succeeded)
    {
        if (options.enablePreparedPrefabCachePreflight)
        {
            const auto preflightBegin = std::chrono::steady_clock::now();
            const auto preflight = PreflightPreparedPrefabCache(
                options.projectRoot,
                database,
                options.maxPreparedPrefabCachePreflightCount,
                options.maxPreparedPrefabCachePreflightDuration,
                options.priorityPreparedPrefabAssetPaths,
                progressSink);
            result.preparedPrefabCachePreflightAttemptCount = preflight.attemptedCount;
            result.preparedPrefabCachePreflightCount = preflight.preparedCount;
            LogStartupTiming(
                "Prepared prefab cache preflight",
                preflightBegin,
                result.preparedPrefabCachePreflightAttemptCount);
        }

        const auto indexBuildBegin = std::chrono::steady_clock::now();
        const bool patchedStartupIndex =
            standardPbrDependencyReady &&
            result.plannedAssetCount == 0u &&
            result.importedAssetCount == 0u &&
            TryPatchStartupAssetPreimportIndexForEmptyPlan(options.projectRoot, startupStampPath, cacheAnalysis);
        if (patchedStartupIndex)
        {
            LogStartupTiming("Startup asset cache index patch", indexBuildBegin);
        }
        else
        {
            ++StartupAssetPreimportFullIndexRebuildCountForTestingStorage();
            const auto startupIndex = BuildStartupAssetPreimportIndex(
                options.projectRoot,
                database,
                cacheAnalysis.loadedIndex ? &*cacheAnalysis.loadedIndex : nullptr);
            LogStartupTiming("Startup asset cache index rebuild", indexBuildBegin);
            if (standardPbrDependencyReady &&
                startupIndex.has_value() &&
                !WriteStartupAssetPreimportIndex(startupStampPath, *startupIndex))
            {
                NLS_LOG_WARNING("[StartupAssetPreimport] Failed to write startup asset preimport cache stamp.");
            }
        }
    }
    result.hadRunningJobsAfterCompletion = tracker.HasRunningJobs();
    result.diagnostics = database.GetDiagnostics();
    return result;
}

#if defined(NLS_ENABLE_TEST_HOOKS)
void ResetStartupAssetPreimportIndexLoadCountForTesting()
{
    StartupAssetPreimportIndexLoadCountForTestingStorage() = 0u;
}

size_t GetStartupAssetPreimportIndexLoadCountForTesting()
{
    return StartupAssetPreimportIndexLoadCountForTestingStorage();
}

void ResetStartupAssetPreimportSourceEnumerationCountForTesting()
{
    StartupAssetPreimportSourceEnumerationCountForTestingStorage() = 0u;
}

size_t GetStartupAssetPreimportSourceEnumerationCountForTesting()
{
    return StartupAssetPreimportSourceEnumerationCountForTestingStorage();
}

void ResetStartupAssetPreimportContentHashReadCountForTesting()
{
    StartupAssetPreimportContentHashReadCountForTestingStorage() = 0u;
}

size_t GetStartupAssetPreimportContentHashReadCountForTesting()
{
    return StartupAssetPreimportContentHashReadCountForTestingStorage();
}

void ResetStartupAssetPreimportFileMetadataQueryCountForTesting()
{
    StartupAssetPreimportFileMetadataQueryCountForTestingStorage() = 0u;
}

size_t GetStartupAssetPreimportFileMetadataQueryCountForTesting()
{
    return StartupAssetPreimportFileMetadataQueryCountForTestingStorage();
}

void ResetStartupAssetPreimportShardWriteCountForTesting()
{
    StartupAssetPreimportShardWriteCountForTestingStorage() = 0u;
}

size_t GetStartupAssetPreimportShardWriteCountForTesting()
{
    return StartupAssetPreimportShardWriteCountForTestingStorage();
}

void ResetStartupAssetPreimportFullIndexRebuildCountForTesting()
{
    StartupAssetPreimportFullIndexRebuildCountForTestingStorage() = 0u;
}

size_t GetStartupAssetPreimportFullIndexRebuildCountForTesting()
{
    return StartupAssetPreimportFullIndexRebuildCountForTestingStorage();
}

void ResetStartupAssetPreimportPatchedIndexWriteCountForTesting()
{
    StartupAssetPreimportPatchedIndexWriteCountForTestingStorage() = 0u;
}

size_t GetStartupAssetPreimportPatchedIndexWriteCountForTesting()
{
    return StartupAssetPreimportPatchedIndexWriteCountForTestingStorage();
}

void ResetStartupAssetPreimportImporterFingerprintComputeCountForTesting()
{
    StartupAssetPreimportImporterFingerprintComputeCountForTestingStorage() = 0u;
}

size_t GetStartupAssetPreimportImporterFingerprintComputeCountForTesting()
{
    return StartupAssetPreimportImporterFingerprintComputeCountForTestingStorage();
}

bool RewriteStartupAssetPreimportIndexForTesting(const std::filesystem::path& projectRoot)
{
    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(projectRoot));
    if (!database.Refresh())
        return false;

    const auto startupIndex = BuildStartupAssetPreimportIndex(projectRoot, database);
    if (!startupIndex.has_value())
        return false;
    return WriteStartupAssetPreimportIndex(GetStartupAssetPreimportStampPath(projectRoot), *startupIndex);
}
#endif
}
