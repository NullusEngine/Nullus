#pragma once

#include "CoreDef.h"
#include "Assets/AssetId.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace NLS::Core::Assets
{
enum class AssetType
{
    Unknown,
    ModelScene,
    Texture,
    Shader,
    Material,
    Audio,
    Scene,
    Prefab
};

NLS_CORE_API const char* ToString(AssetType type);
NLS_CORE_API AssetType AssetTypeFromString(const std::string& text);
NLS_CORE_API std::filesystem::path GetAssetMetaPath(const std::filesystem::path& assetPath);
NLS_CORE_API AssetType InferAssetType(const std::filesystem::path& assetPath);
NLS_CORE_API std::string InferImporterId(AssetType type);
NLS_CORE_API uint32_t GetCurrentImporterVersion(AssetType type);

struct NLS_CORE_API AssetMeta
{
    AssetId id;
    std::string importerId = "unknown";
    uint32_t importerVersion = 1u;
    AssetType assetType = AssetType::Unknown;
    std::map<std::string, std::string> settings;

    static std::optional<AssetMeta> Load(const std::filesystem::path& metaPath);
    static AssetMeta CreateForAsset(const std::filesystem::path& assetPath);

    bool Save(const std::filesystem::path& metaPath) const;
};

inline bool operator==(const AssetMeta& lhs, const AssetMeta& rhs)
{
    return lhs.id == rhs.id &&
        lhs.importerId == rhs.importerId &&
        lhs.importerVersion == rhs.importerVersion &&
        lhs.assetType == rhs.assetType &&
        lhs.settings == rhs.settings;
}

inline bool operator!=(const AssetMeta& lhs, const AssetMeta& rhs)
{
    return !(lhs == rhs);
}
}
