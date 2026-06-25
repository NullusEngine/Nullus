#pragma once

#include "Assets/AssetId.h"
#include "CoreDef.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace NLS::Core::Assets
{
enum class ArtifactType
{
    Unknown,
    Model,
    Mesh,
    Material,
    Texture,
    Skeleton,
    Skin,
    AnimationClip,
    MorphTarget,
    Prefab,
    Scene,
    Shader,
    Audio,
    Count
};

enum class AssetDependencyKind
{
    SourceFileHash,
    SourceAssetGuid,
    ImportedArtifact,
    PathToGuidMapping,
    BuildTarget,
    ImporterVersion,
    PostprocessorVersion,
    PrefabBase,
    NestedPrefab,
    PrefabOverrideTarget,
    RuntimeComponentCapability,
    RawPackageFile
};

struct AssetDependencyRecord
{
    AssetDependencyKind kind = AssetDependencyKind::SourceFileHash;
    std::string value;
    std::string hashOrVersion;
};

struct ImportedArtifact
{
    AssetId sourceAssetId;
    std::string subAssetKey;
    ArtifactType artifactType = ArtifactType::Unknown;
    std::string loaderId;
    std::string targetPlatform;
    std::string artifactPath;
    std::string contentHash;
    std::string displayName;
};

struct NLS_CORE_API ArtifactManifest
{
    AssetId sourceAssetId;
    std::string importerId;
    uint32_t importerVersion = 1u;
    std::string targetPlatform;
    std::string primarySubAssetKey;
    std::vector<ImportedArtifact> subAssets;
    std::vector<AssetDependencyRecord> dependencies;

    const ImportedArtifact* FindPrimaryArtifact() const;
    const ImportedArtifact* FindSubAsset(const std::string& subAssetKey) const;
};

NLS_CORE_API bool IsContentStorageArtifactPath(const std::string& artifactPath);
NLS_CORE_API std::string TryMakePortableContentArtifactPath(const std::string& artifactPath);
NLS_CORE_API bool IsArtifactStorageFileName(const std::string& fileName);
NLS_CORE_API std::filesystem::path BuildArtifactStorageRelativePath(std::string_view storageFileName);
NLS_CORE_API std::string BuildArtifactStorageFileName(std::string_view storageKey);
NLS_CORE_API std::string BuildArtifactStorageFileName(const uint8_t* bytes, size_t byteCount);
NLS_CORE_API void ClearRuntimeArtifactAuthorization();
NLS_CORE_API void RegisterRuntimeAuthorizedArtifactPath(const std::string& artifactPath);
NLS_CORE_API void SetRuntimeArtifactAuthorizationEnabled(bool enabled);
NLS_CORE_API bool IsRuntimeArtifactAuthorizationEnabled();
NLS_CORE_API bool IsRuntimeArtifactPathAuthorized(const std::string& artifactPath);
}
