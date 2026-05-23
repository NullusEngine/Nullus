#include "Assets/AssetVersion.h"

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace NLS::Core::Assets
{
namespace
{
bool IsSameDependency(const AssetDependencyRecord& lhs, const AssetDependencyRecord& rhs)
{
    return lhs.kind == rhs.kind &&
        lhs.value == rhs.value &&
        lhs.hashOrVersion == rhs.hashOrVersion;
}
}

std::string AssetVersion::MakeCacheKey() const
{
    std::ostringstream stream;
    stream << "source=" << sourceContentHash
        << "|meta=" << metaHash
        << "|deps=" << dependencyHash
        << "|importer=" << importerVersion
        << "|postprocessor=" << postprocessorVersion
        << "|target=" << targetPlatform
        << "|artifact=" << artifactHash;
    return stream.str();
}

void AssetDependencyGraph::AddAsset(AssetId asset)
{
    m_dependenciesByOwner.try_emplace(asset);
}

void AssetDependencyGraph::AddDependency(AssetId owner, AssetDependencyRecord dependency)
{
    auto& dependencies = m_dependenciesByOwner[owner];
    const auto alreadyExists = std::any_of(
        dependencies.begin(),
        dependencies.end(),
        [&dependency](const AssetDependencyRecord& existing)
        {
            return IsSameDependency(existing, dependency);
        });
    if (!alreadyExists)
        dependencies.push_back(std::move(dependency));
}

void AssetDependencyGraph::ApplyDependencyChanges(const std::vector<AssetDependencyChange>& changes)
{
    for (const auto& change : changes)
    {
        if (change.change == AssetDependencyChangeKind::Add)
        {
            AddDependency(change.owner, change.dependency);
            continue;
        }

        auto& dependencies = m_dependenciesByOwner[change.owner];
        dependencies.erase(
            std::remove_if(
                dependencies.begin(),
                dependencies.end(),
                [&change](const AssetDependencyRecord& dependency)
                {
                    return IsSameDependency(dependency, change.dependency);
                }),
            dependencies.end());
    }
}

std::vector<AssetId> AssetDependencyGraph::CollectDependents(AssetId changedAsset) const
{
    return CollectDependents(AssetDependencyKind::SourceAssetGuid, changedAsset.ToString());
}

std::vector<AssetId> AssetDependencyGraph::CollectDependents(
    AssetDependencyKind kind,
    const std::string& value) const
{
    std::vector<AssetId> result;
    std::unordered_set<AssetId> visited;
    std::vector<std::pair<AssetDependencyKind, std::string>> frontier {{kind, value}};

    while (!frontier.empty())
    {
        const auto current = frontier.back();
        frontier.pop_back();

        for (const auto& [owner, dependencies] : m_dependenciesByOwner)
        {
            if (visited.find(owner) != visited.end())
                continue;

            const auto dependsOnCurrent = std::any_of(
                dependencies.begin(),
                dependencies.end(),
                [&current](const AssetDependencyRecord& dependency)
                {
                    return dependency.kind == current.first && dependency.value == current.second;
                });

            if (!dependsOnCurrent)
                continue;

            visited.insert(owner);
            result.push_back(owner);
            frontier.push_back({AssetDependencyKind::SourceAssetGuid, owner.ToString()});
            frontier.push_back({AssetDependencyKind::ImportedArtifact, owner.ToString()});
            frontier.push_back({AssetDependencyKind::PrefabBase, owner.ToString()});
            frontier.push_back({AssetDependencyKind::NestedPrefab, owner.ToString()});
            frontier.push_back({AssetDependencyKind::PrefabOverrideTarget, owner.ToString()});
        }
    }

    std::sort(result.begin(), result.end(), [](AssetId lhs, AssetId rhs)
    {
        return lhs.ToString() < rhs.ToString();
    });
    return result;
}

std::vector<AssetId> AssetDependencyGraph::CollectDependents(
    const std::vector<AssetDependencyRecord>& changedDependencies) const
{
    std::vector<AssetId> result;
    std::unordered_set<AssetId> merged;
    for (const auto& dependency : changedDependencies)
    {
        for (const auto& dependent : CollectDependents(dependency.kind, dependency.value))
        {
            if (merged.insert(dependent).second)
                result.push_back(dependent);
        }
    }

    std::sort(result.begin(), result.end(), [](AssetId lhs, AssetId rhs)
    {
        return lhs.ToString() < rhs.ToString();
    });
    return result;
}

std::vector<AssetId> AssetDependencyGraph::CollectPrefabDependents(AssetId prefabAsset) const
{
    std::vector<AssetDependencyRecord> changedDependencies;
    changedDependencies.push_back(MakePrefabBaseDependency(prefabAsset, {}));
    changedDependencies.push_back(MakeNestedPrefabDependency(prefabAsset, {}));
    changedDependencies.push_back(MakePrefabOverrideTargetDependency(prefabAsset, {}));
    changedDependencies.push_back({AssetDependencyKind::SourceAssetGuid, prefabAsset.ToString(), {}});
    return CollectDependents(changedDependencies);
}

const std::vector<AssetDependencyRecord>* AssetDependencyGraph::GetDependencies(AssetId owner) const
{
    const auto found = m_dependenciesByOwner.find(owner);
    if (found == m_dependenciesByOwner.end())
        return nullptr;
    return &found->second;
}

AssetDependencyRecord MakePrefabBaseDependency(AssetId prefabAsset, std::string subAssetKey)
{
    return {AssetDependencyKind::PrefabBase, prefabAsset.ToString(), std::move(subAssetKey)};
}

AssetDependencyRecord MakeNestedPrefabDependency(AssetId prefabAsset, std::string subAssetKey)
{
    return {AssetDependencyKind::NestedPrefab, prefabAsset.ToString(), std::move(subAssetKey)};
}

AssetDependencyRecord MakePrefabOverrideTargetDependency(AssetId prefabAsset, std::string subAssetKey)
{
    return {AssetDependencyKind::PrefabOverrideTarget, prefabAsset.ToString(), std::move(subAssetKey)};
}

AssetHotReloadPolicy GetHotReloadPolicy(ArtifactType artifactType)
{
    switch (artifactType)
    {
    case ArtifactType::Mesh:
    case ArtifactType::Material:
    case ArtifactType::Texture:
    case ArtifactType::Shader:
        return AssetHotReloadPolicy::ReloadInPlace;
    case ArtifactType::Prefab:
    case ArtifactType::Model:
    case ArtifactType::Skeleton:
    case ArtifactType::Skin:
    case ArtifactType::AnimationClip:
    case ArtifactType::MorphTarget:
        return AssetHotReloadPolicy::MarkInstancesDirty;
    case ArtifactType::Scene:
    case ArtifactType::Audio:
    case ArtifactType::Unknown:
    default:
        return AssetHotReloadPolicy::RequiresExplicitReload;
    }
}
}
