#pragma once

#include "Assets/ArtifactManifest.h"
#include "CoreDef.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace NLS::Core::Assets
{
struct NLS_CORE_API AssetVersion
{
    std::string sourceContentHash;
    std::string metaHash;
    std::string dependencyHash;
    uint32_t importerVersion = 0u;
    uint32_t postprocessorVersion = 0u;
    std::string targetPlatform;
    std::string artifactHash;

    std::string MakeCacheKey() const;
};

enum class AssetDependencyChangeKind
{
    Add,
    Remove
};

struct NLS_CORE_API AssetDependencyChange
{
    AssetDependencyChangeKind change = AssetDependencyChangeKind::Add;
    AssetId owner;
    AssetDependencyRecord dependency;
};

class NLS_CORE_API AssetDependencyGraph
{
public:
    void AddAsset(AssetId asset);
    void AddDependency(AssetId owner, AssetDependencyRecord dependency);
    void ApplyDependencyChanges(const std::vector<AssetDependencyChange>& changes);
    std::vector<AssetId> CollectDependents(AssetId changedAsset) const;
    std::vector<AssetId> CollectDependents(AssetDependencyKind kind, const std::string& value) const;
    std::vector<AssetId> CollectDependents(const std::vector<AssetDependencyRecord>& changedDependencies) const;
    std::vector<AssetId> CollectPrefabDependents(AssetId prefabAsset) const;
    const std::vector<AssetDependencyRecord>* GetDependencies(AssetId owner) const;

private:
    std::unordered_map<AssetId, std::vector<AssetDependencyRecord>> m_dependenciesByOwner;
};

enum class AssetHotReloadPolicy
{
    ReloadInPlace,
    MarkInstancesDirty,
    RequiresExplicitReload
};

NLS_CORE_API AssetHotReloadPolicy GetHotReloadPolicy(ArtifactType artifactType);

NLS_CORE_API AssetDependencyRecord MakePrefabBaseDependency(AssetId prefabAsset, std::string subAssetKey);
NLS_CORE_API AssetDependencyRecord MakeNestedPrefabDependency(AssetId prefabAsset, std::string subAssetKey);
NLS_CORE_API AssetDependencyRecord MakePrefabOverrideTargetDependency(AssetId prefabAsset, std::string subAssetKey);
}
