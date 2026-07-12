#include "Assets/AssetThumbnailCache.h"

#include "Assets/ArtifactManifest.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/AssetMeta.h"
#include "Assets/EditorAssetPath.h"
#include "Image.h"

#include <Json/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace NLS::Editor::Assets
{
namespace
{
constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

constexpr size_t kCacheWriteMutexStripeCount = 64u;

#if defined(NLS_ENABLE_TEST_HOOKS)
std::atomic<size_t> g_canonicalPathAttemptCountForTesting {0u};
std::atomic<size_t> g_containmentStampAttemptCountForTesting {0u};
std::atomic<size_t> g_metadataFileLoadCountForTesting {0u};
std::atomic<size_t> g_cacheEvaluationCountForTesting {0u};
#endif

using CanonicalPathCache = std::unordered_map<std::string, std::optional<std::filesystem::path>>;

std::filesystem::path NormalizePath(const std::filesystem::path& path);

struct MetadataCacheEntry
{
    std::filesystem::file_time_type lastWriteTime {};
    uintmax_t byteSize = 0u;
    nlohmann::json metadata;
};

struct FilesystemContainmentStamp
{
    std::filesystem::file_type type = std::filesystem::file_type::none;
};

struct CacheEntryContainmentCacheEntry
{
    std::array<FilesystemContainmentStamp, 6u> stamps {};
    bool contained = false;
};

struct CacheRootContainmentCacheEntry
{
    FilesystemContainmentStamp projectRootStamp;
    FilesystemContainmentStamp cacheRootStamp;
    bool contained = false;
};

bool operator==(
    const FilesystemContainmentStamp& lhs,
    const FilesystemContainmentStamp& rhs)
{
    return lhs.type == rhs.type;
}

bool operator==(
    const CacheEntryContainmentCacheEntry& lhs,
    const CacheEntryContainmentCacheEntry& rhs)
{
    return lhs.stamps == rhs.stamps &&
        lhs.contained == rhs.contained;
}

std::mutex& MetadataCacheMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, MetadataCacheEntry>& MetadataCache()
{
    static std::unordered_map<std::string, MetadataCacheEntry> cache;
    return cache;
}

std::mutex& CacheEntryContainmentCacheMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, CacheEntryContainmentCacheEntry>& CacheEntryContainmentCache()
{
    static std::unordered_map<std::string, CacheEntryContainmentCacheEntry> cache;
    return cache;
}

std::mutex& CacheRootContainmentCacheMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, CacheRootContainmentCacheEntry>& CacheRootContainmentCache()
{
    static std::unordered_map<std::string, CacheRootContainmentCacheEntry> cache;
    return cache;
}

std::string MetadataCacheKey(const std::filesystem::path& path)
{
    return path.lexically_normal().generic_string();
}

#ifdef _WIN32
bool PathHasWindowsReparsePoint(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u;
}
#endif

FilesystemContainmentStamp BuildFilesystemContainmentStamp(const std::filesystem::path& path)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    g_containmentStampAttemptCountForTesting.fetch_add(1u, std::memory_order_relaxed);
#endif
    FilesystemContainmentStamp stamp;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    const bool hasReparsePoint = PathHasWindowsReparsePoint(path);
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND ||
            error == ERROR_PATH_NOT_FOUND ||
            error == ERROR_NOT_READY)
        {
            stamp.type = std::filesystem::file_type::not_found;
        }
        return stamp;
    }

    if (hasReparsePoint)
        stamp.type = std::filesystem::file_type::symlink;
    else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0u)
        stamp.type = std::filesystem::file_type::directory;
    else
        stamp.type = std::filesystem::file_type::regular;
    return stamp;
#else
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error)
    {
        std::error_code existsError;
        if (!std::filesystem::exists(path, existsError) && !existsError)
            stamp.type = std::filesystem::file_type::not_found;
        return stamp;
    }

    stamp.type = status.type();
    return stamp;
#endif
}

bool CacheEntryContainmentStampIsCacheable(const FilesystemContainmentStamp& stamp)
{
    return stamp.type == std::filesystem::file_type::directory ||
        stamp.type == std::filesystem::file_type::regular ||
        stamp.type == std::filesystem::file_type::not_found;
}

std::optional<CacheEntryContainmentCacheEntry> BuildCacheEntryContainmentCacheEntry(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& cacheRoot,
    const AssetThumbnailCacheEntry& entry,
    const bool contained)
{
    const auto normalizedProjectRoot = NormalizePath(projectRoot);
    const auto imageParent = entry.imagePath.parent_path();
    const auto metadataParent = entry.metadataPath.parent_path();
    const auto projectRootStamp = BuildFilesystemContainmentStamp(normalizedProjectRoot);
    const auto cacheRootStamp = BuildFilesystemContainmentStamp(cacheRoot);
    const auto imageParentStamp = BuildFilesystemContainmentStamp(imageParent);
    const auto imageStamp = BuildFilesystemContainmentStamp(entry.imagePath);
    const auto metadataParentStamp = imageParent == metadataParent
        ? imageParentStamp
        : BuildFilesystemContainmentStamp(metadataParent);
    const auto metadataStamp = BuildFilesystemContainmentStamp(entry.metadataPath);

    CacheEntryContainmentCacheEntry cacheEntry;
    cacheEntry.stamps = {
        projectRootStamp,
        cacheRootStamp,
        imageParentStamp,
        imageStamp,
        metadataParentStamp,
        metadataStamp
    };
    cacheEntry.contained = contained;

    if (!std::all_of(
            cacheEntry.stamps.begin(),
            cacheEntry.stamps.end(),
            CacheEntryContainmentStampIsCacheable))
    {
        return std::nullopt;
    }
    return cacheEntry;
}

std::string CacheEntryContainmentCacheKey(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& cacheRoot,
    const AssetThumbnailCacheEntry& entry)
{
    return MetadataCacheKey(projectRoot) + "|" +
        MetadataCacheKey(cacheRoot) + "|" +
        entry.cacheKey + "|" +
        MetadataCacheKey(entry.imagePath) + "|" +
        MetadataCacheKey(entry.metadataPath);
}

std::string CacheRootContainmentCacheKey(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& cacheRoot)
{
    return MetadataCacheKey(projectRoot) + "|" + MetadataCacheKey(cacheRoot);
}

void InvalidateMetadataCacheEntry(const std::filesystem::path& path)
{
    std::lock_guard<std::mutex> lock(MetadataCacheMutex());
    MetadataCache().erase(MetadataCacheKey(path));
}

std::array<std::mutex, kCacheWriteMutexStripeCount>& CacheWriteMutexStripes()
{
    static std::array<std::mutex, kCacheWriteMutexStripeCount> mutexes;
    return mutexes;
}

std::mutex& CacheWriteMutexForKey(const std::string& cacheKey)
{
    return CacheWriteMutexStripes()[std::hash<std::string> {}(cacheKey) % kCacheWriteMutexStripeCount];
}

std::string ThumbnailTelemetryPathForRequest(const AssetThumbnailRequest& request)
{
    return request.sourceAssetPath + "|" + request.subAssetKey;
}

void RecordThumbnailCacheEvaluationTelemetry(
    const NLS::Core::Assets::ArtifactLoadTelemetryStage stage,
    const std::chrono::steady_clock::time_point begin,
    const AssetThumbnailRequest& request,
    const size_t byteCount = 0u)
{
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        stage,
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin),
        byteCount,
        ThumbnailTelemetryPathForRequest(request)
    });
}

void RecordThumbnailCacheTelemetry(
    const NLS::Core::Assets::ArtifactLoadTelemetryStage stage,
    const std::chrono::steady_clock::time_point begin,
    const std::string& path,
    const size_t byteCount = 0u)
{
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        stage,
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin),
        byteCount,
        path
    });
}

bool WriteNewFileExclusive(
    const std::filesystem::path& path,
    const std::vector<uint8_t>& bytes)
{
#ifdef _WIN32
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0u,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    bool success = true;
    size_t offset = 0u;
    while (offset < bytes.size())
    {
        const auto remaining = bytes.size() - offset;
        const auto chunkSize = static_cast<DWORD>(
            std::min<size_t>(remaining, static_cast<size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0u;
        if (!WriteFile(file, bytes.data() + offset, chunkSize, &written, nullptr) ||
            written != chunkSize)
        {
            success = false;
            break;
        }
        offset += static_cast<size_t>(written);
    }

    if (!CloseHandle(file))
        success = false;
    return success;
#else
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int file = open(path.c_str(), flags, 0600);
    if (file < 0)
        return false;

    bool success = true;
    size_t offset = 0u;
    while (offset < bytes.size())
    {
        const auto remaining = bytes.size() - offset;
        const ssize_t written = write(file, bytes.data() + offset, remaining);
        if (written < 0)
        {
            if (errno == EINTR)
                continue;
            success = false;
            break;
        }
        if (written == 0)
        {
            success = false;
            break;
        }
        offset += static_cast<size_t>(written);
    }

    if (close(file) != 0)
        success = false;
    return success;
#endif
}

bool ReplaceFileWithTemp(
    const std::filesystem::path& tempPath,
    const std::filesystem::path& targetPath)
{
#ifdef _WIN32
    return MoveFileExW(
        tempPath.c_str(),
        targetPath.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    std::filesystem::rename(tempPath, targetPath, error);
    return !error;
#endif
}

std::filesystem::path NormalizePath(const std::filesystem::path& path)
{
    return path.lexically_normal();
}

void HashBytes(uint64_t& hash, const std::string& value)
{
    for (const auto byte : value)
    {
        hash ^= static_cast<unsigned char>(byte);
        hash *= kFnvPrime;
    }
    hash ^= 0xffu;
    hash *= kFnvPrime;
}

std::string HashParts(const std::vector<std::string>& parts)
{
    uint64_t hash = kFnvOffset;
    for (const auto& part : parts)
        HashBytes(hash, part);

    constexpr char kHexDigits[] = "0123456789abcdef";
    std::string result(16u, '0');
    for (size_t index = 0u; index < result.size(); ++index)
    {
        const auto shift = static_cast<unsigned>((result.size() - index - 1u) * 4u);
        result[index] = kHexDigits[(hash >> shift) & 0xfu];
    }
    return result;
}

struct AssetThumbnailKindDescriptor
{
    AssetThumbnailKind kind = AssetThumbnailKind::GenericPreview;
    const char* label = "";
};

struct AssetThumbnailCacheStatusDescriptor
{
    AssetThumbnailCacheStatus status = AssetThumbnailCacheStatus::Missing;
    const char* token = "";
};

constexpr std::array<AssetThumbnailKindDescriptor, kAssetThumbnailKindCount> kAssetThumbnailKindDescriptors {{
    { AssetThumbnailKind::Icon, "icon" },
    { AssetThumbnailKind::Texture, "texture" },
    { AssetThumbnailKind::MaterialSphere, "material-sphere" },
    { AssetThumbnailKind::ModelPreview, "model-preview" },
    { AssetThumbnailKind::PrefabPreview, "prefab-preview" },
    { AssetThumbnailKind::GenericPreview, "generic-preview" }
}};

constexpr std::array<AssetThumbnailCacheStatusDescriptor, kAssetThumbnailCacheStatusCount> kAssetThumbnailCacheStatusDescriptors {{
    { AssetThumbnailCacheStatus::Fresh, "fresh" },
    { AssetThumbnailCacheStatus::Stale, "stale" },
    { AssetThumbnailCacheStatus::Missing, "missing" },
    { AssetThumbnailCacheStatus::Failed, "failed" }
}};

constexpr bool AssetThumbnailKindDescriptorsAreExhaustive()
{
    if (kAssetThumbnailKindDescriptors.size() != kAssetThumbnailKindCount)
        return false;

    std::array<bool, kAssetThumbnailKindCount> seen {};
    for (const auto& descriptor : kAssetThumbnailKindDescriptors)
    {
        const auto index = static_cast<size_t>(descriptor.kind);
        if (index >= kAssetThumbnailKindCount || seen[index])
            return false;
        seen[index] = true;
    }

    for (const bool covered : seen)
    {
        if (!covered)
            return false;
    }
    return true;
}

static_assert(AssetThumbnailKindDescriptorsAreExhaustive());

constexpr bool AssetThumbnailCacheStatusDescriptorsAreExhaustive()
{
    if (kAssetThumbnailCacheStatusDescriptors.size() != kAssetThumbnailCacheStatusCount)
        return false;

    std::array<bool, kAssetThumbnailCacheStatusCount> seen {};
    for (const auto& descriptor : kAssetThumbnailCacheStatusDescriptors)
    {
        const auto index = static_cast<size_t>(descriptor.status);
        if (index >= kAssetThumbnailCacheStatusCount || seen[index] || descriptor.token == nullptr || descriptor.token[0] == '\0')
            return false;
        seen[index] = true;
    }

    for (const bool covered : seen)
    {
        if (!covered)
            return false;
    }
    return true;
}

static_assert(AssetThumbnailCacheStatusDescriptorsAreExhaustive());

std::string KindToString(const AssetThumbnailKind kind)
{
    const auto index = static_cast<size_t>(kind);
    if (index >= kAssetThumbnailKindCount)
        return "invalid";

    for (const auto& descriptor : kAssetThumbnailKindDescriptors)
    {
        if (descriptor.kind == kind)
            return descriptor.label;
    }
    return "invalid";
}

std::vector<AssetThumbnailFreshnessInput> SortedFreshnessInputs(
    std::vector<AssetThumbnailFreshnessInput> inputs)
{
    std::sort(
        inputs.begin(),
        inputs.end(),
        [](const AssetThumbnailFreshnessInput& left, const AssetThumbnailFreshnessInput& right)
        {
            if (left.name != right.name)
                return left.name < right.name;
            return left.stamp < right.stamp;
        });
    return inputs;
}

std::vector<std::string> BuildAssetThumbnailCommonKeyParts(const AssetThumbnailRequest& request)
{
    std::vector<std::string> parts;
    parts.reserve(11u);
    parts = {
        request.assetId.ToString(),
        NormalizeEditorAssetPath(request.sourceAssetPath),
        request.subAssetKey,
        NormalizeEditorAssetPath(request.artifactPath),
        KindToString(request.kind),
        std::to_string(request.requestedSize),
        request.previewRendererVersion,
        request.settingsFingerprint,
        request.dependencyStamp,
        request.colorSpaceMode,
        request.hdrMode
    };
    return parts;
}

std::string BuildAssetThumbnailStablePathKey(const std::vector<std::string>& commonParts)
{
    return HashParts(commonParts);
}

std::string BuildAssetThumbnailStablePathKey(const AssetThumbnailRequest& request)
{
    return BuildAssetThumbnailStablePathKey(BuildAssetThumbnailCommonKeyParts(request));
}

std::filesystem::path CacheRoot(const std::filesystem::path& projectRoot)
{
    return NormalizePath(projectRoot) / "Library" / "AssetThumbnails";
}

bool IsPathInside(const std::filesystem::path& candidate, const std::filesystem::path& root)
{
    const auto normalizedCandidate = NormalizePath(candidate);
    const auto normalizedRoot = NormalizePath(root);
    if (normalizedCandidate == normalizedRoot)
        return true;

    const auto relative = normalizedCandidate.lexically_relative(normalizedRoot);
    if (relative.empty() || relative.is_absolute())
        return false;

    for (const auto& part : relative)
    {
        if (part == "..")
            return false;
    }
    return true;
}

std::optional<std::filesystem::path> TryWeaklyCanonicalEditorPathCached(
    const std::filesystem::path& path,
    CanonicalPathCache& cache)
{
    const auto key = MetadataCacheKey(path);
    const auto found = cache.find(key);
    if (found != cache.end())
        return found->second;

#if defined(NLS_ENABLE_TEST_HOOKS)
    g_canonicalPathAttemptCountForTesting.fetch_add(1u, std::memory_order_relaxed);
#endif
    auto canonical = TryWeaklyCanonicalEditorPath(path);
    cache.emplace(key, canonical);
    return canonical;
}

bool IsPhysicallyInside(
    const std::filesystem::path& candidate,
    const std::filesystem::path& root)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    g_canonicalPathAttemptCountForTesting.fetch_add(2u, std::memory_order_relaxed);
#endif
    const auto canonicalCandidate = TryWeaklyCanonicalEditorPath(candidate);
    const auto canonicalRoot = TryWeaklyCanonicalEditorPath(root);
    return canonicalCandidate.has_value() &&
        canonicalRoot.has_value() &&
        IsPathInside(*canonicalCandidate, *canonicalRoot);
}

bool IsPhysicallyInside(
    const std::filesystem::path& candidate,
    const std::filesystem::path& root,
    CanonicalPathCache& cache)
{
    const auto canonicalCandidate = TryWeaklyCanonicalEditorPathCached(candidate, cache);
    const auto canonicalRoot = TryWeaklyCanonicalEditorPathCached(root, cache);
    return canonicalCandidate.has_value() &&
        canonicalRoot.has_value() &&
        IsPathInside(*canonicalCandidate, *canonicalRoot);
}

bool IsCacheRootPhysicallyInsideProject(const std::filesystem::path& projectRoot)
{
    return IsPhysicallyInside(CacheRoot(projectRoot), NormalizePath(projectRoot));
}

bool IsCacheRootPhysicallyInsideProject(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& cacheRoot,
    CanonicalPathCache& cache)
{
    return IsPhysicallyInside(cacheRoot, NormalizePath(projectRoot), cache);
}

bool IsCacheCandidateContained(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& candidate)
{
    const auto root = CacheRoot(projectRoot);
    return IsPathInside(candidate, root) &&
        IsCacheRootPhysicallyInsideProject(projectRoot) &&
        IsPhysicallyInside(candidate, root);
}

bool IsCacheCandidateContained(
    const std::filesystem::path& candidate,
    const std::filesystem::path& cacheRoot,
    CanonicalPathCache& cache)
{
    return IsPathInside(candidate, cacheRoot) &&
        IsPhysicallyInside(candidate, cacheRoot, cache);
}

bool AreCacheEntryPathsContained(
    const std::filesystem::path& projectRoot,
    const AssetThumbnailCacheEntry& entry)
{
    const auto cacheRoot = CacheRoot(projectRoot);
    CanonicalPathCache cache;
    if (!IsCacheRootPhysicallyInsideProject(projectRoot, cacheRoot, cache))
        return false;

    return IsCacheCandidateContained(entry.imagePath.parent_path(), cacheRoot, cache) &&
        IsCacheCandidateContained(entry.imagePath, cacheRoot, cache) &&
        IsCacheCandidateContained(entry.metadataPath.parent_path(), cacheRoot, cache) &&
        IsCacheCandidateContained(entry.metadataPath, cacheRoot, cache);
}

bool AreCacheEntryPathsLexicallyContained(
    const std::filesystem::path& cacheRoot,
    const AssetThumbnailCacheEntry& entry)
{
    return IsPathInside(entry.imagePath.parent_path(), cacheRoot) &&
        IsPathInside(entry.imagePath, cacheRoot) &&
        IsPathInside(entry.metadataPath.parent_path(), cacheRoot) &&
        IsPathInside(entry.metadataPath, cacheRoot);
}

bool IsCacheRootPhysicallyInsideProjectCachedForRead(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& cacheRoot,
    const CacheEntryContainmentCacheEntry& cacheEntry)
{
    const auto key = CacheRootContainmentCacheKey(projectRoot, cacheRoot);
    {
        std::lock_guard<std::mutex> lock(CacheRootContainmentCacheMutex());
        const auto found = CacheRootContainmentCache().find(key);
        if (found != CacheRootContainmentCache().end() &&
            found->second.projectRootStamp == cacheEntry.stamps[0] &&
            found->second.cacheRootStamp == cacheEntry.stamps[1])
        {
            return found->second.contained;
        }
    }

    CanonicalPathCache cache;
    const bool contained = IsCacheRootPhysicallyInsideProject(projectRoot, cacheRoot, cache);
    {
        std::lock_guard<std::mutex> lock(CacheRootContainmentCacheMutex());
        CacheRootContainmentCache()[key] = CacheRootContainmentCacheEntry {
            cacheEntry.stamps[0],
            cacheEntry.stamps[1],
            contained
        };
    }
    return contained;
}

std::optional<bool> TryValidateCacheEntryContainmentFromCacheableStamps(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& cacheRoot,
    const AssetThumbnailCacheEntry& entry,
    const CacheEntryContainmentCacheEntry& cacheEntry)
{
    if (!std::all_of(
            cacheEntry.stamps.begin(),
            cacheEntry.stamps.end(),
            CacheEntryContainmentStampIsCacheable))
    {
        return std::nullopt;
    }

    if (!AreCacheEntryPathsLexicallyContained(cacheRoot, entry))
        return false;

    return IsCacheRootPhysicallyInsideProjectCachedForRead(projectRoot, cacheRoot, cacheEntry);
}

bool AreCacheEntryPathsContainedCachedForRead(
    const std::filesystem::path& projectRoot,
    const AssetThumbnailCacheEntry& entry,
    const std::string& telemetryPath)
{
    auto telemetryBegin = std::chrono::steady_clock::now();
    const auto cacheRoot = CacheRoot(projectRoot);
    const auto cacheKey = CacheEntryContainmentCacheKey(projectRoot, cacheRoot, entry);
    RecordThumbnailCacheTelemetry(
        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateResolveEntryContainmentKey,
        telemetryBegin,
        telemetryPath);
    std::optional<CacheEntryContainmentCacheEntry> cachedEntry;
    {
        std::lock_guard<std::mutex> lock(CacheEntryContainmentCacheMutex());
        const auto found = CacheEntryContainmentCache().find(cacheKey);
        if (found != CacheEntryContainmentCache().end())
            cachedEntry = found->second;
    }
    if (cachedEntry.has_value())
    {
        telemetryBegin = std::chrono::steady_clock::now();
        if (const auto currentCacheEntry =
                BuildCacheEntryContainmentCacheEntry(projectRoot, cacheRoot, entry, cachedEntry->contained);
            currentCacheEntry.has_value() && *cachedEntry == *currentCacheEntry)
        {
            RecordThumbnailCacheTelemetry(
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateResolveEntryContainmentStamp,
                telemetryBegin,
                telemetryPath);
            return cachedEntry->contained;
        }
        RecordThumbnailCacheTelemetry(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateResolveEntryContainmentStamp,
            telemetryBegin,
            telemetryPath);
    }

    telemetryBegin = std::chrono::steady_clock::now();
    auto refreshedCacheEntry = BuildCacheEntryContainmentCacheEntry(projectRoot, cacheRoot, entry, false);
    RecordThumbnailCacheTelemetry(
        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateResolveEntryContainmentStamp,
        telemetryBegin,
        telemetryPath);

    telemetryBegin = std::chrono::steady_clock::now();
    const auto fastContained = refreshedCacheEntry.has_value()
        ? TryValidateCacheEntryContainmentFromCacheableStamps(projectRoot, cacheRoot, entry, *refreshedCacheEntry)
        : std::nullopt;
    const bool contained = fastContained.has_value()
        ? *fastContained
        : AreCacheEntryPathsContained(projectRoot, entry);
    RecordThumbnailCacheTelemetry(
        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateResolveEntryContainmentValidate,
        telemetryBegin,
        telemetryPath);
    if (refreshedCacheEntry.has_value())
    {
        refreshedCacheEntry->contained = contained;
        std::lock_guard<std::mutex> lock(CacheEntryContainmentCacheMutex());
        CacheEntryContainmentCache()[cacheKey] = *refreshedCacheEntry;
    }
    return contained;
}

std::optional<nlohmann::json> LoadMetadata(const std::filesystem::path& path)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    g_metadataFileLoadCountForTesting.fetch_add(1u, std::memory_order_relaxed);
#endif
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;

    auto root = nlohmann::json::parse(input, nullptr, false);
    if (!root.is_object())
        return std::nullopt;
    return root;
}

std::optional<nlohmann::json> LoadMetadataCached(
    const std::filesystem::path& path,
    const std::filesystem::file_time_type lastWriteTime,
    const uintmax_t byteSize,
    const bool cacheable)
{
    if (!cacheable)
        return LoadMetadata(path);

    const auto key = NormalizePath(path).generic_string();
    {
        std::lock_guard<std::mutex> lock(MetadataCacheMutex());
        const auto found = MetadataCache().find(key);
        if (found != MetadataCache().end() &&
            found->second.lastWriteTime == lastWriteTime &&
            found->second.byteSize == byteSize)
        {
            return found->second.metadata;
        }
    }

    auto metadata = LoadMetadata(path);
    if (!metadata.has_value())
        return std::nullopt;

    {
        std::lock_guard<std::mutex> lock(MetadataCacheMutex());
        auto& cache = MetadataCache();
        if (cache.size() >= 1024u)
            cache.clear();
        cache[key] = MetadataCacheEntry {
            lastWriteTime,
            byteSize,
            *metadata
        };
    }
    return metadata;
}

uint64_t FileSizeOrZero(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? 0ull : static_cast<uint64_t>(size);
}

struct DiskCachePruneEntry
{
    std::filesystem::path metadataPath;
    std::filesystem::path imagePath;
    uint64_t byteSize = 0u;
    std::filesystem::file_time_type lastWriteTime {};
};

std::filesystem::path ResolvePruneImagePath(
    const std::filesystem::path& metadataPath,
    const nlohmann::json& metadata)
{
    const auto cacheKey = metadata.value("cacheKey", std::string {});
    if (!cacheKey.empty())
        return NormalizePath(metadataPath.parent_path() / (cacheKey + ".png"));

    auto imagePath = metadataPath;
    imagePath.replace_extension(".png");
    return NormalizePath(imagePath);
}

std::optional<DiskCachePruneEntry> BuildPruneEntry(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& metadataPath)
{
    if (!IsCacheCandidateContained(projectRoot, metadataPath))
        return std::nullopt;

    const auto metadata = LoadMetadata(metadataPath);
    if (!metadata.has_value())
        return std::nullopt;

    const auto imagePath = ResolvePruneImagePath(metadataPath, *metadata);
    if (!IsCacheCandidateContained(projectRoot, imagePath))
        return std::nullopt;

    std::error_code error;
    DiskCachePruneEntry entry;
    entry.metadataPath = NormalizePath(metadataPath);
    entry.imagePath = imagePath;
    entry.byteSize = FileSizeOrZero(entry.metadataPath) + FileSizeOrZero(entry.imagePath);
    entry.lastWriteTime = std::filesystem::last_write_time(entry.metadataPath, error);
    if (error)
        entry.lastWriteTime = std::filesystem::file_time_type::min();
    return entry;
}

void RemovePruneEntryFiles(
    const std::filesystem::path& projectRoot,
    const DiskCachePruneEntry& entry)
{
    std::error_code error;
    if (IsCacheCandidateContained(projectRoot, entry.imagePath))
        std::filesystem::remove(entry.imagePath, error);
    error.clear();
    if (IsCacheCandidateContained(projectRoot, entry.metadataPath))
        std::filesystem::remove(entry.metadataPath, error);
}

bool ReadFilePrefix(const std::filesystem::path& path, std::vector<uint8_t>& bytes, const size_t size)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;

    bytes.resize(size);
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return input.gcount() == static_cast<std::streamsize>(size);
}

uint32_t ReadBigEndianUInt32(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24u) |
        (static_cast<uint32_t>(data[1]) << 16u) |
        (static_cast<uint32_t>(data[2]) << 8u) |
        static_cast<uint32_t>(data[3]);
}

bool HasValidPngCacheHeader(
    const std::filesystem::path& imagePath,
    const uint32_t requestedSize)
{
    constexpr std::array<uint8_t, 8u> kPngSignature {
        0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au
    };
    constexpr size_t kPngSignatureSize = kPngSignature.size();
    constexpr size_t kPngIhdrPrefixSize = 33u;

    std::vector<uint8_t> header;
    if (!ReadFilePrefix(imagePath, header, kPngIhdrPrefixSize))
        return false;

    if (!std::equal(kPngSignature.begin(), kPngSignature.end(), header.begin()))
        return false;

    const auto ihdrLength = ReadBigEndianUInt32(header.data() + kPngSignatureSize);
    if (ihdrLength != 13u)
        return false;

    if (header[kPngSignatureSize + 4u] != 'I' ||
        header[kPngSignatureSize + 5u] != 'H' ||
        header[kPngSignatureSize + 6u] != 'D' ||
        header[kPngSignatureSize + 7u] != 'R')
    {
        return false;
    }

    const auto width = ReadBigEndianUInt32(header.data() + kPngSignatureSize + 8u);
    const auto height = ReadBigEndianUInt32(header.data() + kPngSignatureSize + 12u);
    if (width == 0u || height == 0u)
        return false;
    if (requestedSize > 0u && (width > requestedSize || height > requestedSize))
        return false;

    return true;
}

std::optional<std::string> HashFileForStorage(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;

    uint64_t hash = kFnvOffset;
    std::array<char, 4096u> buffer {};
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        for (std::streamsize index = 0; index < count; ++index)
        {
            hash ^= static_cast<unsigned char>(buffer[static_cast<size_t>(index)]);
            hash *= kFnvPrime;
        }
    }

    if (!input.eof())
        return std::nullopt;

    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

bool CachedImageMatchesMetadata(
    const std::filesystem::path& imagePath,
    const nlohmann::json& metadata,
    const AssetThumbnailCacheIntegrityMode integrityMode)
{
    if (!metadata.contains("imageSize") || !metadata.contains("imageHash"))
        return false;

    std::error_code error;
    const auto actualSize = std::filesystem::file_size(imagePath, error);
    if (error)
        return false;

    const auto expectedSize = metadata.value("imageSize", std::uintmax_t {});
    if (actualSize != expectedSize)
        return false;

    if (integrityMode == AssetThumbnailCacheIntegrityMode::Fast)
        return true;

    const auto expectedHash = metadata.value("imageHash", std::string {});
    if (expectedHash.empty())
        return false;

    const auto actualHash = HashFileForStorage(imagePath);
    return actualHash.has_value() && *actualHash == expectedHash;
}

bool CachedImageMetadataHasIdentity(const nlohmann::json& metadata)
{
    if (!metadata.contains("imageSize") || !metadata.contains("imageHash"))
        return false;
    if (metadata.value("imageSize", std::uintmax_t {}) == 0u)
        return false;
    return !metadata.value("imageHash", std::string {}).empty();
}

std::string FileStampForFreshness(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return "missing";

    error.clear();
    const auto writeTime = std::filesystem::last_write_time(path, error);
    if (error)
        return "missing";

    return std::to_string(size) + ":" +
        std::to_string(static_cast<std::intmax_t>(writeTime.time_since_epoch().count()));
}

std::optional<std::filesystem::path> ResolveMetadataArtifactPath(
    const std::filesystem::path& projectRoot,
    const std::string& assetId,
    const std::string& artifactPath)
{
    if (projectRoot.empty() || assetId.empty() || artifactPath.empty())
        return std::nullopt;

    const auto normalizedProjectRoot = NormalizePath(projectRoot);
    const auto artifactRoot = NormalizePath(normalizedProjectRoot / "Library" / "Artifacts");
    if (!NLS::Core::Assets::IsContentStorageArtifactPath(artifactPath))
        return std::nullopt;
    auto candidate = NormalizePath(std::filesystem::path(artifactPath));
    if (candidate.is_relative())
        candidate = NormalizePath(normalizedProjectRoot / candidate);

    if (!IsPathInside(candidate, artifactRoot) || !IsPhysicallyInside(candidate, artifactRoot))
        return std::nullopt;
    return candidate;
}

std::optional<std::string> CurrentMetadataFreshnessStamp(
    const AssetThumbnailRequest& request,
    const nlohmann::json& metadata,
    const std::string& name)
{
    const auto assetId = metadata.value("assetId", request.assetId.ToString());
    if (name == "artifact-db" || name == "artifact-manifest")
    {
        return FileStampForFreshness(NormalizePath(request.projectRoot / "Library" / "ArtifactDB" / "data.mdb"));
    }

    if (name == "artifact-file")
    {
        const auto artifactPath = metadata.value("artifactPath", std::string {});
        const auto resolvedArtifactPath =
            ResolveMetadataArtifactPath(request.projectRoot, assetId, artifactPath);
        if (!resolvedArtifactPath.has_value())
            return std::nullopt;
        return FileStampForFreshness(*resolvedArtifactPath);
    }

    if (name == "source-file")
    {
        const auto sourceAssetPath = metadata.value("sourceAssetPath", std::string {});
        if (sourceAssetPath.empty())
            return std::nullopt;
        auto sourcePath = NormalizePath(std::filesystem::path(sourceAssetPath));
        if (sourcePath.is_relative())
            sourcePath = NormalizePath(request.projectRoot / sourcePath);
        if (!IsPathInside(sourcePath, NormalizePath(request.projectRoot)) ||
            !IsPhysicallyInside(sourcePath, NormalizePath(request.projectRoot)))
        {
            return std::nullopt;
        }
        return FileStampForFreshness(sourcePath);
    }

    if (name == "source-meta")
    {
        const auto sourceAssetPath = metadata.value("sourceAssetPath", std::string {});
        if (sourceAssetPath.empty())
            return std::nullopt;
        auto sourcePath = NormalizePath(std::filesystem::path(sourceAssetPath));
        if (sourcePath.is_relative())
            sourcePath = NormalizePath(request.projectRoot / sourcePath);
        const auto projectRoot = NormalizePath(request.projectRoot);
        if (!IsPathInside(sourcePath, projectRoot) || !IsPhysicallyInside(sourcePath, projectRoot))
            return std::nullopt;
        const auto metaPath = NLS::Core::Assets::GetAssetMetaPath(sourcePath);
        if (!IsPathInside(metaPath, projectRoot) || !IsPhysicallyInside(metaPath, projectRoot))
            return std::nullopt;
        return FileStampForFreshness(metaPath);
    }

    return std::nullopt;
}

bool MetadataFreshnessInputsAreCurrent(
    const AssetThumbnailRequest& request,
    const nlohmann::json& metadata)
{
    const auto inputs = metadata.find("freshnessInputs");
    if (inputs == metadata.end() || !inputs->is_array())
        return true;

    for (const auto& input : *inputs)
    {
        if (!input.is_object())
            continue;

        const auto name = input.value("name", std::string {});
        if (name != "artifact-file" &&
            name != "artifact-db" &&
            name != "artifact-manifest" &&
            name != "source-meta" &&
            name != "source-file")
        {
            continue;
        }

        const auto expectedStamp = input.value("stamp", std::string {});
        const auto currentStamp = CurrentMetadataFreshnessStamp(request, metadata, name);
        if (!currentStamp.has_value() || *currentStamp != expectedStamp)
            return false;
    }
    return true;
}

bool WriteAssetThumbnailCacheFileForEntry(
    const std::filesystem::path& projectRoot,
    const AssetThumbnailCacheEntry& entry,
    const std::filesystem::path& path,
    const std::vector<uint8_t>& bytes)
{
    const auto normalizedPath = NormalizePath(path);
    if (normalizedPath != entry.imagePath &&
        normalizedPath != entry.metadataPath)
    {
        return false;
    }

    auto& writeMutex = CacheWriteMutexForKey(entry.cacheKey);
    std::lock_guard<std::mutex> writeLock(writeMutex);

    std::error_code error;
    std::filesystem::create_directories(normalizedPath.parent_path(), error);
    if (error)
        return false;

    if (!AreCacheEntryPathsContained(projectRoot, entry) ||
        !IsCacheCandidateContained(projectRoot, normalizedPath.parent_path()) ||
        !IsCacheCandidateContained(projectRoot, normalizedPath))
    {
        return false;
    }

    static std::atomic<uint64_t> tempCounter {0u};
    const auto suffix =
        std::to_string(
            static_cast<unsigned long long>(
                std::chrono::steady_clock::now().time_since_epoch().count())) +
        "." +
        std::to_string(std::hash<std::thread::id> {}(std::this_thread::get_id())) +
        "." +
        std::to_string(tempCounter.fetch_add(1u, std::memory_order_relaxed));
    const auto tempPath = NormalizePath(
        normalizedPath.parent_path() /
        ("." + normalizedPath.filename().generic_string() + "." + entry.cacheKey + "." + suffix + ".tmp"));
    if (!IsCacheCandidateContained(projectRoot, tempPath))
        return false;

    if (!WriteNewFileExclusive(tempPath, bytes))
    {
        std::filesystem::remove(tempPath, error);
        return false;
    }

    if (!IsCacheCandidateContained(projectRoot, tempPath))
    {
        std::filesystem::remove(tempPath, error);
        return false;
    }

    if (!ReplaceFileWithTemp(tempPath, normalizedPath))
    {
        std::filesystem::remove(tempPath, error);
        return false;
    }

    error.clear();
    const bool wroteRegularFile = std::filesystem::is_regular_file(normalizedPath, error);
    if (error || !wroteRegularFile || !IsCacheCandidateContained(projectRoot, normalizedPath))
    {
        std::filesystem::remove(normalizedPath, error);
        return false;
    }
    if (normalizedPath == entry.metadataPath)
        InvalidateMetadataCacheEntry(normalizedPath);
    return true;
}

bool WriteAssetThumbnailCacheMetadataForEntry(
    const AssetThumbnailRequest& metadataRequest,
    const AssetThumbnailCacheEntry& entry,
    const AssetThumbnailCacheStatus status,
    const std::string& diagnostic)
{
    if (status != AssetThumbnailCacheStatus::Fresh)
    {
        if (!AreCacheEntryPathsContained(metadataRequest.projectRoot, entry))
            return false;

        std::error_code removeError;
        std::filesystem::remove(entry.imagePath, removeError);
        if (removeError)
            return false;
    }

    nlohmann::json freshness = nlohmann::json::array();
    for (const auto& input : SortedFreshnessInputs(metadataRequest.freshnessInputs))
    {
        freshness.push_back({
            {"name", input.name},
            {"stamp", input.stamp}
        });
    }

    nlohmann::json metadata {
        {"cacheKey", entry.cacheKey},
        {"status", AssetThumbnailCacheStatusStorageToken(status)},
        {"assetId", metadataRequest.assetId.ToString()},
        {"sourceAssetPath", NormalizeEditorAssetPath(metadataRequest.sourceAssetPath)},
        {"subAssetKey", metadataRequest.subAssetKey},
        {"artifactPath", NormalizeEditorAssetPath(metadataRequest.artifactPath)},
        {"thumbnailKind", KindToString(metadataRequest.kind)},
        {"requestedSize", metadataRequest.requestedSize},
        {"previewRendererVersion", metadataRequest.previewRendererVersion},
        {"settingsFingerprint", metadataRequest.settingsFingerprint},
        {"dependencyStamp", metadataRequest.dependencyStamp},
        {"colorSpaceMode", metadataRequest.colorSpaceMode},
        {"hdrMode", metadataRequest.hdrMode},
        {"freshnessInputs", freshness},
        {"diagnostic", diagnostic}
    };

    if (status == AssetThumbnailCacheStatus::Fresh)
    {
        if (!HasValidPngCacheHeader(entry.imagePath, metadataRequest.requestedSize))
            return false;

        std::error_code error;
        const auto imageSize = std::filesystem::file_size(entry.imagePath, error);
        const auto imageHash = error ? std::optional<std::string> {} : HashFileForStorage(entry.imagePath);
        if (error || !imageHash.has_value())
            return false;

        metadata["imageSize"] = imageSize;
        metadata["imageHash"] = *imageHash;
    }

    const auto text = metadata.dump(2);
    return WriteAssetThumbnailCacheFileForEntry(
        metadataRequest.projectRoot,
        entry,
        entry.metadataPath,
        std::vector<uint8_t>(text.begin(), text.end()));
}
}

#if defined(NLS_ENABLE_TEST_HOOKS)
void ResetAssetThumbnailCacheCanonicalPathAttemptCountForTesting()
{
    g_canonicalPathAttemptCountForTesting.store(0u, std::memory_order_relaxed);
}

size_t GetAssetThumbnailCacheCanonicalPathAttemptCountForTesting()
{
    return g_canonicalPathAttemptCountForTesting.load(std::memory_order_relaxed);
}

void ResetAssetThumbnailCacheContainmentStampAttemptCountForTesting()
{
    g_containmentStampAttemptCountForTesting.store(0u, std::memory_order_relaxed);
}

size_t GetAssetThumbnailCacheContainmentStampAttemptCountForTesting()
{
    return g_containmentStampAttemptCountForTesting.load(std::memory_order_relaxed);
}

void ResetAssetThumbnailCacheMetadataFileLoadCountForTesting()
{
    g_metadataFileLoadCountForTesting.store(0u, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(MetadataCacheMutex());
    MetadataCache().clear();
}

size_t GetAssetThumbnailCacheMetadataFileLoadCountForTesting()
{
    return g_metadataFileLoadCountForTesting.load(std::memory_order_relaxed);
}

size_t GetAssetThumbnailCacheMetadataCacheEntryCountForTesting()
{
    std::lock_guard<std::mutex> lock(MetadataCacheMutex());
    return MetadataCache().size();
}

void ResetAssetThumbnailCacheEvaluationCountForTesting()
{
    g_cacheEvaluationCountForTesting.store(0u, std::memory_order_relaxed);
}

size_t GetAssetThumbnailCacheEvaluationCountForTesting()
{
    return g_cacheEvaluationCountForTesting.load(std::memory_order_relaxed);
}
#endif

std::string BuildAssetThumbnailCacheKey(
    std::vector<std::string> parts,
    const std::vector<AssetThumbnailFreshnessInput>& freshnessInputs)
{
    parts.reserve(parts.size() + freshnessInputs.size() * 2u);
    for (const auto& input : SortedFreshnessInputs(freshnessInputs))
    {
        parts.push_back(input.name);
        parts.push_back(input.stamp);
    }
    return HashParts(parts);
}

std::string BuildAssetThumbnailCacheKey(const AssetThumbnailRequest& request)
{
    return BuildAssetThumbnailCacheKey(
        BuildAssetThumbnailCommonKeyParts(request),
        request.freshnessInputs);
}

std::optional<AssetThumbnailCacheEntry> BuildAssetThumbnailCacheEntryUnchecked(
    const AssetThumbnailRequest& request)
{
    if (request.projectRoot.empty() || !request.assetId.IsValid())
        return std::nullopt;

    auto commonParts = BuildAssetThumbnailCommonKeyParts(request);
    const auto stableKey = BuildAssetThumbnailStablePathKey(commonParts);
    const auto root = CacheRoot(request.projectRoot);
    const auto directory = root / stableKey.substr(0u, 2u);
    AssetThumbnailCacheEntry entry;
    entry.cacheKey = BuildAssetThumbnailCacheKey(std::move(commonParts), request.freshnessInputs);
    entry.imagePath = NormalizePath(directory / (entry.cacheKey + ".png"));
    entry.metadataPath = NormalizePath(directory / (entry.cacheKey + ".json"));
    return entry;
}

std::optional<AssetThumbnailCacheEntry> ResolveAssetThumbnailCacheEntryPathForRead(
    const AssetThumbnailRequest& request)
{
    const auto telemetryPath = ThumbnailTelemetryPathForRequest(request);
    auto telemetryBegin = std::chrono::steady_clock::now();
    auto entry = BuildAssetThumbnailCacheEntryUnchecked(request);
    RecordThumbnailCacheTelemetry(
        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateResolveEntryBuild,
        telemetryBegin,
        telemetryPath);
    if (!entry.has_value())
        return std::nullopt;

    if (!AreCacheEntryPathsLexicallyContained(CacheRoot(request.projectRoot), *entry))
        return std::nullopt;
    return entry;
}

std::optional<AssetThumbnailCacheEntry> ResolveAssetThumbnailCacheEntryForRead(
    const AssetThumbnailRequest& request)
{
    auto entry = ResolveAssetThumbnailCacheEntryPathForRead(request);
    if (!entry.has_value())
        return std::nullopt;

    const auto telemetryPath = ThumbnailTelemetryPathForRequest(request);
    if (!AreCacheEntryPathsContainedCachedForRead(request.projectRoot, *entry, telemetryPath))
    {
        return std::nullopt;
    }
    return entry;
}

std::optional<AssetThumbnailCacheEntry> ResolveAssetThumbnailCacheEntry(
    const AssetThumbnailRequest& request)
{
    auto entry = BuildAssetThumbnailCacheEntryUnchecked(request);
    if (!entry.has_value())
        return std::nullopt;

    if (!AreCacheEntryPathsContained(request.projectRoot, *entry))
    {
        return std::nullopt;
    }
    return entry;
}

bool IsAssetThumbnailCachePathContained(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& candidate)
{
    if (projectRoot.empty() || candidate.empty())
        return false;

    return IsCacheCandidateContained(projectRoot, candidate);
}

const std::array<AssetThumbnailCacheStatus, kAssetThumbnailCacheStatusCount>& AssetThumbnailCacheStatusValues()
{
    static const std::array<AssetThumbnailCacheStatus, kAssetThumbnailCacheStatusCount> values = []
    {
        std::array<AssetThumbnailCacheStatus, kAssetThumbnailCacheStatusCount> result {};
        for (size_t index = 0u; index < kAssetThumbnailCacheStatusDescriptors.size(); ++index)
            result[index] = kAssetThumbnailCacheStatusDescriptors[index].status;
        return result;
    }();
    return values;
}

const char* AssetThumbnailCacheStatusStorageToken(const AssetThumbnailCacheStatus status)
{
    const auto index = static_cast<size_t>(status);
    if (index >= kAssetThumbnailCacheStatusCount)
        return "invalid";

    for (const auto& descriptor : kAssetThumbnailCacheStatusDescriptors)
    {
        if (descriptor.status == status)
            return descriptor.token;
    }
    return "invalid";
}

std::optional<AssetThumbnailCacheStatus> AssetThumbnailCacheStatusFromStorageToken(
    const std::string& value)
{
    for (const auto& descriptor : kAssetThumbnailCacheStatusDescriptors)
    {
        if (value == descriptor.token)
            return descriptor.status;
    }
    return std::nullopt;
}

AssetThumbnailCacheEvaluation EvaluateAssetThumbnailCache(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheIntegrityMode integrityMode)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    g_cacheEvaluationCountForTesting.fetch_add(1u, std::memory_order_relaxed);
#endif
    AssetThumbnailCacheEvaluation result;
    auto telemetryBegin = std::chrono::steady_clock::now();
    result.entry = ResolveAssetThumbnailCacheEntryPathForRead(request);
    RecordThumbnailCacheEvaluationTelemetry(
        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateResolveEntry,
        telemetryBegin,
        request);
    if (!result.entry.has_value())
    {
        result.diagnostic = "thumbnail-cache-path-invalid";
        return result;
    }

    std::optional<bool> fastImageExists;
    bool fastImageStatHadError = false;
    if (integrityMode == AssetThumbnailCacheIntegrityMode::Fast)
    {
        std::error_code imageError;
        telemetryBegin = std::chrono::steady_clock::now();
        fastImageExists = std::filesystem::is_regular_file(result.entry->imagePath, imageError);
        fastImageStatHadError = static_cast<bool>(imageError);
        RecordThumbnailCacheEvaluationTelemetry(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateImageStat,
            telemetryBegin,
            request);
        if (!fastImageStatHadError && *fastImageExists)
        {
            if (!AreCacheEntryPathsContainedCachedForRead(
                    request.projectRoot,
                    *result.entry,
                    ThumbnailTelemetryPathForRequest(request)))
            {
                result.entry.reset();
                result.diagnostic = "thumbnail-cache-path-invalid";
                return result;
            }
            result.status = AssetThumbnailCacheStatus::Fresh;
            return result;
        }
    }

    std::error_code error;
    telemetryBegin = std::chrono::steady_clock::now();
    const bool metadataExists = std::filesystem::is_regular_file(result.entry->metadataPath, error);
    uintmax_t metadataByteCount = 0u;
    std::filesystem::file_time_type metadataLastWriteTime {};
    bool metadataCacheable = false;
    if (!error && metadataExists)
    {
        std::error_code sizeError;
        metadataByteCount = std::filesystem::file_size(result.entry->metadataPath, sizeError);
        if (sizeError)
            metadataByteCount = 0u;
        std::error_code timeError;
        metadataLastWriteTime = std::filesystem::last_write_time(result.entry->metadataPath, timeError);
        metadataCacheable = !sizeError && !timeError;
    }
    RecordThumbnailCacheEvaluationTelemetry(
        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateMetadataStat,
        telemetryBegin,
        request);
    if (error || !metadataExists)
        return result;

    if (!AreCacheEntryPathsContainedCachedForRead(
            request.projectRoot,
            *result.entry,
            ThumbnailTelemetryPathForRequest(request)))
    {
        result.entry.reset();
        result.diagnostic = "thumbnail-cache-path-invalid";
        return result;
    }

    telemetryBegin = std::chrono::steady_clock::now();
    const auto metadata = LoadMetadataCached(
        result.entry->metadataPath,
        metadataLastWriteTime,
        metadataByteCount,
        metadataCacheable);
    RecordThumbnailCacheEvaluationTelemetry(
        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateMetadataLoad,
        telemetryBegin,
        request,
        metadata.has_value() ? static_cast<size_t>(metadataByteCount) : 0u);
    if (!metadata.has_value())
    {
        result.status = AssetThumbnailCacheStatus::Stale;
        result.diagnostic = "thumbnail-cache-metadata-invalid";
        return result;
    }

    const auto storedKey = metadata->value("cacheKey", std::string {});
    if (storedKey != result.entry->cacheKey)
    {
        result.status = AssetThumbnailCacheStatus::Stale;
        return result;
    }

    result.status = AssetThumbnailCacheStatusFromStorageToken(metadata->value("status", std::string {}))
        .value_or(AssetThumbnailCacheStatus::Missing);
    result.diagnostic = metadata->value("diagnostic", std::string {});
    if ((result.status == AssetThumbnailCacheStatus::Fresh ||
            result.status == AssetThumbnailCacheStatus::Failed))
    {
        telemetryBegin = std::chrono::steady_clock::now();
        const bool freshnessCurrent =
            MetadataFreshnessInputsAreCurrent(request, *metadata);
        RecordThumbnailCacheEvaluationTelemetry(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateFreshness,
            telemetryBegin,
            request,
            request.freshnessInputs.size());
        if (!freshnessCurrent)
        {
            result.status = AssetThumbnailCacheStatus::Stale;
            result.diagnostic = "thumbnail-cache-freshness-stale";
            return result;
        }
    }

    if (result.status == AssetThumbnailCacheStatus::Failed)
        return result;

    bool imageExists = false;
    bool imageStatHadError = false;
    if (fastImageExists.has_value())
    {
        imageExists = *fastImageExists;
        imageStatHadError = fastImageStatHadError;
    }
    else
    {
        error.clear();
        telemetryBegin = std::chrono::steady_clock::now();
        imageExists = std::filesystem::is_regular_file(result.entry->imagePath, error);
        imageStatHadError = static_cast<bool>(error);
        RecordThumbnailCacheEvaluationTelemetry(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateImageStat,
            telemetryBegin,
            request);
    }
    if (imageStatHadError || !imageExists)
    {
        result.status = AssetThumbnailCacheStatus::Missing;
        result.diagnostic.clear();
        return result;
    }

    if (result.status == AssetThumbnailCacheStatus::Fresh)
    {
        telemetryBegin = std::chrono::steady_clock::now();
        const bool imageValid =
            integrityMode == AssetThumbnailCacheIntegrityMode::Fast
                ? CachedImageMetadataHasIdentity(*metadata)
                : (HasValidPngCacheHeader(result.entry->imagePath, request.requestedSize) &&
                    CachedImageMatchesMetadata(result.entry->imagePath, *metadata, integrityMode));
        RecordThumbnailCacheEvaluationTelemetry(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateImageValidate,
            telemetryBegin,
            request);
        if (!imageValid)
        {
            result.status = AssetThumbnailCacheStatus::Stale;
            result.diagnostic = "thumbnail-cache-image-invalid";
            return result;
        }
    }

    if (result.status == AssetThumbnailCacheStatus::Missing ||
        result.status == AssetThumbnailCacheStatus::Stale)
    {
        result.status = AssetThumbnailCacheStatus::Stale;
    }
    return result;
}

bool WriteAssetThumbnailCacheMetadata(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheStatus status,
    const std::string& diagnostic)
{
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    if (!entry.has_value())
        return false;
    return WriteAssetThumbnailCacheMetadataForEntry(request, *entry, status, diagnostic);
}

bool WriteAssetThumbnailCacheMetadata(
    const AssetThumbnailRequest& metadataRequest,
    const AssetThumbnailCacheEntry& entry,
    const AssetThumbnailCacheStatus status,
    const std::string& diagnostic)
{
    if (!AreCacheEntryPathsContained(metadataRequest.projectRoot, entry))
        return false;
    return WriteAssetThumbnailCacheMetadataForEntry(metadataRequest, entry, status, diagnostic);
}

bool WriteAssetThumbnailCacheFile(
    const AssetThumbnailRequest& request,
    const std::filesystem::path& path,
    const std::vector<uint8_t>& bytes)
{
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    if (!entry.has_value())
        return false;
    return WriteAssetThumbnailCacheFileForEntry(request.projectRoot, *entry, path, bytes);
}

AssetThumbnailDiskCachePruneResult PruneAssetThumbnailDiskCache(
    const std::filesystem::path& projectRoot,
    const AssetThumbnailDiskCachePruneOptions& options)
{
    AssetThumbnailDiskCachePruneResult result;
    if (projectRoot.empty())
        return result;

    const auto root = CacheRoot(projectRoot);
    if (!IsCacheRootPhysicallyInsideProject(projectRoot))
        return result;

    std::error_code error;
    if (!std::filesystem::exists(root, error) || error)
        return result;

    std::vector<DiskCachePruneEntry> entries;
    for (std::filesystem::recursive_directory_iterator iterator(root, error), end; iterator != end; iterator.increment(error))
    {
        if (error)
        {
            error.clear();
            continue;
        }

        const auto& path = iterator->path();
        if (!iterator->is_regular_file(error) || error || path.extension() != ".json")
        {
            error.clear();
            continue;
        }

        auto entry = BuildPruneEntry(projectRoot, path);
        if (!entry.has_value())
            continue;
        entries.push_back(std::move(*entry));
    }

    std::sort(
        entries.begin(),
        entries.end(),
        [](const DiskCachePruneEntry& left, const DiskCachePruneEntry& right)
        {
            if (left.lastWriteTime != right.lastWriteTime)
                return left.lastWriteTime > right.lastWriteTime;
            return left.metadataPath.generic_string() > right.metadataPath.generic_string();
        });

    result.scannedEntries = entries.size();

    uint64_t retainedBytes = 0u;
    size_t retainedEntries = 0u;
    for (const auto& entry : entries)
    {
        const bool withinEntryBudget = retainedEntries < options.maxEntries;
        const bool withinByteBudget =
            options.maxBytes == UINT64_MAX ||
            retainedBytes + entry.byteSize <= options.maxBytes;
        if (withinEntryBudget && withinByteBudget)
        {
            ++retainedEntries;
            retainedBytes += entry.byteSize;
            continue;
        }

        RemovePruneEntryFiles(projectRoot, entry);
        ++result.removedEntries;
        result.removedBytes += entry.byteSize;
    }

    result.remainingEntries = retainedEntries;
    result.remainingBytes = retainedBytes;
    return result;
}
}
