#pragma once

#include "Assets/AssetId.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Editor::Assets
{
enum class AssetThumbnailKind
{
    Icon,
    Texture,
    MaterialSphere,
    ModelPreview,
    PrefabPreview,
    GenericPreview,
    Count
};

inline constexpr size_t kAssetThumbnailKindCount = static_cast<size_t>(AssetThumbnailKind::Count);

enum class AssetThumbnailCacheStatus
{
    Fresh,
    Stale,
    Missing,
    Failed,
    Count
};

inline constexpr size_t kAssetThumbnailCacheStatusCount = static_cast<size_t>(AssetThumbnailCacheStatus::Count);

enum class AssetThumbnailCacheIntegrityMode
{
    Fast,
    Full
};

struct AssetThumbnailFreshnessInput
{
    std::string name;
    std::string stamp;
};

struct AssetThumbnailRequest
{
    std::filesystem::path projectRoot;
    NLS::Core::Assets::AssetId assetId;
    std::string sourceAssetPath;
    std::string subAssetKey;
    std::string artifactPath;
    AssetThumbnailKind kind = AssetThumbnailKind::GenericPreview;
    uint32_t requestedSize = 96u;
    std::string settingsFingerprint;
    std::vector<AssetThumbnailFreshnessInput> freshnessInputs;
};

struct AssetThumbnailCacheEntry
{
    std::string cacheKey;
    std::filesystem::path imagePath;
    std::filesystem::path metadataPath;
};

struct AssetThumbnailCacheEvaluation
{
    AssetThumbnailCacheStatus status = AssetThumbnailCacheStatus::Missing;
    std::optional<AssetThumbnailCacheEntry> entry;
    std::string diagnostic;
};

std::string BuildAssetThumbnailCacheKey(const AssetThumbnailRequest& request);

std::optional<AssetThumbnailCacheEntry> ResolveAssetThumbnailCacheEntry(
    const AssetThumbnailRequest& request);

bool IsAssetThumbnailCachePathContained(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& candidate);

const std::array<AssetThumbnailCacheStatus, kAssetThumbnailCacheStatusCount>& AssetThumbnailCacheStatusValues();
const char* AssetThumbnailCacheStatusStorageToken(AssetThumbnailCacheStatus status);
std::optional<AssetThumbnailCacheStatus> AssetThumbnailCacheStatusFromStorageToken(
    const std::string& value);

AssetThumbnailCacheEvaluation EvaluateAssetThumbnailCache(
    const AssetThumbnailRequest& request,
    AssetThumbnailCacheIntegrityMode integrityMode = AssetThumbnailCacheIntegrityMode::Full);

bool WriteAssetThumbnailCacheMetadata(
    const AssetThumbnailRequest& request,
    AssetThumbnailCacheStatus status,
    const std::string& diagnostic);

bool WriteAssetThumbnailCacheFile(
    const AssetThumbnailRequest& request,
    const std::filesystem::path& path,
    const std::vector<uint8_t>& bytes);
}
