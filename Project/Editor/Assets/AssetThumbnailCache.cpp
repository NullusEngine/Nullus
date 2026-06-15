#include "Assets/AssetThumbnailCache.h"

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
        request.settingsFingerprint
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
        request.settingsFingerprint
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

    nlohmann::json freshness = nlohmann::json::array();
    for (const auto& input : SortedFreshnessInputs(request.freshnessInputs))
    {
        freshness.push_back({
            {"name", input.name},
            {"stamp", input.stamp}
        });
    }

    nlohmann::json metadata {
        {"cacheKey", entry->cacheKey},
        {"status", AssetThumbnailCacheStatusStorageToken(status)},
        {"assetId", request.assetId.ToString()},
        {"sourceAssetPath", NormalizeEditorAssetPath(request.sourceAssetPath)},
        {"subAssetKey", request.subAssetKey},
        {"artifactPath", NormalizeEditorAssetPath(request.artifactPath)},
        {"thumbnailKind", KindToString(request.kind)},
        {"requestedSize", request.requestedSize},
        {"settingsFingerprint", request.settingsFingerprint},
        {"freshnessInputs", freshness},
        {"diagnostic", diagnostic}
    };

    if (status == AssetThumbnailCacheStatus::Fresh)
    {
        if (!HasValidPngCacheHeader(entry->imagePath, request.requestedSize))
            return false;

        std::error_code error;
        const auto imageSize = std::filesystem::file_size(entry->imagePath, error);
        const auto imageHash = error ? std::optional<std::string> {} : HashFileForStorage(entry->imagePath);
        if (error || !imageHash.has_value())
            return false;

        metadata["imageSize"] = imageSize;
        metadata["imageHash"] = *imageHash;
    }

    const auto text = metadata.dump(2);
    return WriteAssetThumbnailCacheFile(
        request,
        entry->metadataPath,
        std::vector<uint8_t>(text.begin(), text.end()));
}

bool WriteAssetThumbnailCacheFile(
    const AssetThumbnailRequest& request,
    const std::filesystem::path& path,
    const std::vector<uint8_t>& bytes)
{
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    if (!entry.has_value())
        return false;

    const auto normalizedPath = NormalizePath(path);
    if (normalizedPath != entry->imagePath &&
        normalizedPath != entry->metadataPath)
    {
        return false;
    }

    auto& writeMutex = CacheWriteMutexForKey(entry->cacheKey);
    std::lock_guard<std::mutex> writeLock(writeMutex);

    std::error_code error;
    std::filesystem::create_directories(normalizedPath.parent_path(), error);
    if (error)
        return false;

    if (!AreCacheEntryPathsContained(request.projectRoot, *entry) ||
        !IsCacheCandidateContained(request.projectRoot, normalizedPath.parent_path()) ||
        !IsCacheCandidateContained(request.projectRoot, normalizedPath))
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
        ("." + normalizedPath.filename().generic_string() + "." + entry->cacheKey + "." + suffix + ".tmp"));
    if (!IsCacheCandidateContained(request.projectRoot, tempPath))
        return false;

    {
        std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
        if (!output)
            return false;
        if (!bytes.empty())
        {
            output.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        }
        if (!output)
            return false;
    }

    if (!IsCacheCandidateContained(request.projectRoot, tempPath))
    {
        std::filesystem::remove(tempPath, error);
        return false;
    }

    error.clear();
    if (std::filesystem::exists(normalizedPath, error))
    {
        if (error)
        {
            std::filesystem::remove(tempPath, error);
            return false;
        }
        std::filesystem::remove(normalizedPath, error);
        if (error)
        {
            std::filesystem::remove(tempPath, error);
            return false;
        }
    }

    error.clear();
    std::filesystem::rename(tempPath, normalizedPath, error);
    if (error)
    {
        std::filesystem::remove(tempPath, error);
        return false;
    }

    error.clear();
    const bool wroteRegularFile = std::filesystem::is_regular_file(normalizedPath, error);
    if (error || !wroteRegularFile || !IsCacheCandidateContained(request.projectRoot, normalizedPath))
    {
        std::filesystem::remove(normalizedPath, error);
        return false;
    }
    return true;
}
}
