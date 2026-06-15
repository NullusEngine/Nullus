#pragma once

#include "Assets/AssetId.h"
#include "CoreDef.h"

#include <string>
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
}
