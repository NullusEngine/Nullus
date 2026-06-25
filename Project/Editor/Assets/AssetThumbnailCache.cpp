#include "Assets/AssetThumbnailCache.h"

#include "Assets/ArtifactManifest.h"
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

std::array<std::mutex, kCacheWriteMutexStripeCount>& CacheWriteMutexStripes()
{
    static std::array<std::mutex, kCacheWriteMutexStripeCount> mutexes;
    return mutexes;
}

std::mutex& CacheWriteMutexForKey(const std::string& cacheKey)
{
    return CacheWriteMutexStripes()[std::hash<std::string> {}(cacheKey) % kCacheWriteMutexStripeCount];
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

    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
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

std::string BuildAssetThumbnailStablePathKey(const AssetThumbnailRequest& request)
{
    return HashParts({
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
    });
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

bool IsPhysicallyInside(
    const std::filesystem::path& candidate,
    const std::filesystem::path& root)
{
    const auto canonicalCandidate = TryWeaklyCanonicalEditorPath(candidate);
    const auto canonicalRoot = TryWeaklyCanonicalEditorPath(root);
    return canonicalCandidate.has_value() &&
        canonicalRoot.has_value() &&
        IsPathInside(*canonicalCandidate, *canonicalRoot);
}

bool IsCacheRootPhysicallyInsideProject(const std::filesystem::path& projectRoot)
{
    return IsPhysicallyInside(CacheRoot(projectRoot), NormalizePath(projectRoot));
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

bool AreCacheEntryPathsContained(
    const std::filesystem::path& projectRoot,
    const AssetThumbnailCacheEntry& entry)
{
    return IsCacheCandidateContained(projectRoot, entry.imagePath.parent_path()) &&
        IsCacheCandidateContained(projectRoot, entry.imagePath) &&
        IsCacheCandidateContained(projectRoot, entry.metadataPath.parent_path()) &&
        IsCacheCandidateContained(projectRoot, entry.metadataPath);
}

std::optional<nlohmann::json> LoadMetadata(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;

    auto root = nlohmann::json::parse(input, nullptr, false);
    if (!root.is_object())
        return std::nullopt;
    return root;
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
    return true;
}

bool WriteAssetThumbnailCacheMetadataForEntry(
    const AssetThumbnailRequest& metadataRequest,
    const AssetThumbnailCacheEntry& entry,
    const AssetThumbnailCacheStatus status,
    const std::string& diagnostic)
{
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

std::string BuildAssetThumbnailCacheKey(const AssetThumbnailRequest& request)
{
    std::vector<std::string> parts {
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

    for (const auto& input : SortedFreshnessInputs(request.freshnessInputs))
    {
        parts.push_back(input.name);
        parts.push_back(input.stamp);
    }
    return HashParts(parts);
}

std::optional<AssetThumbnailCacheEntry> ResolveAssetThumbnailCacheEntry(
    const AssetThumbnailRequest& request)
{
    if (request.projectRoot.empty() || !request.assetId.IsValid())
        return std::nullopt;

    const auto stableKey = BuildAssetThumbnailStablePathKey(request);
    const auto root = CacheRoot(request.projectRoot);
    const auto directory = root / stableKey.substr(0u, 2u);
    AssetThumbnailCacheEntry entry;
    entry.cacheKey = BuildAssetThumbnailCacheKey(request);
    entry.imagePath = NormalizePath(directory / (entry.cacheKey + ".png"));
    entry.metadataPath = NormalizePath(directory / (entry.cacheKey + ".json"));

    if (!AreCacheEntryPathsContained(request.projectRoot, entry))
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
    AssetThumbnailCacheEvaluation result;
    result.entry = ResolveAssetThumbnailCacheEntry(request);
    if (!result.entry.has_value())
    {
        result.diagnostic = "thumbnail-cache-path-invalid";
        return result;
    }

    std::error_code error;
    const bool metadataExists = std::filesystem::is_regular_file(result.entry->metadataPath, error);
    if (error || !metadataExists)
        return result;

    const auto metadata = LoadMetadata(result.entry->metadataPath);
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
            result.status == AssetThumbnailCacheStatus::Failed) &&
        !MetadataFreshnessInputsAreCurrent(request, *metadata))
    {
        result.status = AssetThumbnailCacheStatus::Stale;
        result.diagnostic = "thumbnail-cache-freshness-stale";
        return result;
    }

    if (result.status == AssetThumbnailCacheStatus::Failed)
        return result;

    error.clear();
    const bool imageExists = std::filesystem::is_regular_file(result.entry->imagePath, error);
    if (error || !imageExists)
    {
        result.status = AssetThumbnailCacheStatus::Missing;
        result.diagnostic.clear();
        return result;
    }

    if (result.status == AssetThumbnailCacheStatus::Fresh &&
        (!HasValidPngCacheHeader(result.entry->imagePath, request.requestedSize) ||
         !CachedImageMatchesMetadata(result.entry->imagePath, *metadata, integrityMode)))
    {
        result.status = AssetThumbnailCacheStatus::Stale;
        result.diagnostic = "thumbnail-cache-image-invalid";
        return result;
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
