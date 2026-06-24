#include "RuntimeAssetManifestStartup.h"

#include "Assets/ArtifactDatabase.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Guid.h"

#include <algorithm>
#include <queue>
#include <unordered_set>

namespace NLS::Game::RuntimeAssets
{
namespace
{
    constexpr const char* kRuntimeTargetPlatform = "win64";

    std::string LoaderIdForArtifactType(const NLS::Core::Assets::ArtifactType artifactType)
    {
        using NLS::Core::Assets::ArtifactType;
        switch (artifactType)
        {
        case ArtifactType::Model: return "model";
        case ArtifactType::Mesh: return "mesh";
        case ArtifactType::Material: return "material";
        case ArtifactType::Texture: return "texture";
        case ArtifactType::Skeleton: return "skeleton";
        case ArtifactType::Skin: return "skin";
        case ArtifactType::AnimationClip: return "animationClip";
        case ArtifactType::MorphTarget: return "morphTarget";
        case ArtifactType::Prefab: return "prefab";
        case ArtifactType::Scene: return "scene";
        case ArtifactType::Shader: return "shader";
        case ArtifactType::Audio: return "audio";
        case ArtifactType::Unknown:
        case ArtifactType::Count:
        default:
            return {};
        }
    }

    bool IsContentHashBoundToArtifactPath(
        const std::string& artifactPath,
        const std::string& contentHash)
    {
        if (contentHash.rfind("sha256:", 0u) != 0u)
            return false;

        const auto fileName = std::filesystem::path(artifactPath).filename().generic_string();
        return NLS::Core::Assets::IsArtifactStorageFileName(fileName) &&
            contentHash == "sha256:" + fileName;
    }

    bool IsValidRuntimeArtifactEntry(
        const NLS::Core::Assets::ArtifactType artifactType,
        const std::string& loaderId,
        const std::string& artifactPath,
        const std::string& contentHash)
    {
        const auto expectedLoaderId = LoaderIdForArtifactType(artifactType);
        return !expectedLoaderId.empty() &&
            loaderId == expectedLoaderId &&
            !artifactPath.empty() &&
            IsContentHashBoundToArtifactPath(artifactPath, contentHash) &&
            NLS::Core::Assets::IsContentStorageArtifactPath(artifactPath);
    }

    void RegisterRuntimeManifestArtifactAuthorizations(
        const NLS::Engine::Assets::RuntimeAssetManifest& manifest)
    {
        NLS::Core::Assets::ClearRuntimeArtifactAuthorization();
        for (const auto& entry : manifest.entries)
            NLS::Core::Assets::RegisterRuntimeAuthorizedArtifactPath(entry.artifactPath);
        for (const auto& pack : manifest.assetPacks)
        {
            for (const auto& entry : pack.entries)
                NLS::Core::Assets::RegisterRuntimeAuthorizedArtifactPath(entry.artifactPath);
        }
        NLS::Core::Assets::SetRuntimeArtifactAuthorizationEnabled(true);
    }

    bool IsMaterialEntry(
        const NLS::Core::Assets::ArtifactType artifactType,
        const std::string& loaderId)
    {
        return artifactType == NLS::Core::Assets::ArtifactType::Material || loaderId == "material";
    }

    bool IsRuntimeDependencyKind(const NLS::Core::Assets::AssetDependencyKind kind)
    {
        return kind == NLS::Core::Assets::AssetDependencyKind::ImportedArtifact ||
            kind == NLS::Core::Assets::AssetDependencyKind::PrefabBase ||
            kind == NLS::Core::Assets::AssetDependencyKind::NestedPrefab;
    }

    std::optional<NLS::Engine::Assets::RuntimeAssetRef> ToRuntimeDependencyRef(
        const NLS::Core::Assets::AssetDependencyRecord& dependency)
    {
        if (!IsRuntimeDependencyKind(dependency.kind))
            return std::nullopt;

        const auto parsedGuid = NLS::Guid::TryParse(dependency.value);
        if (!parsedGuid.has_value() || dependency.hashOrVersion.empty())
            return std::nullopt;

        return NLS::Engine::Assets::RuntimeAssetRef {
            NLS::Core::Assets::AssetId(*parsedGuid),
            dependency.hashOrVersion
        };
    }

    std::vector<NLS::Engine::Assets::RuntimeAssetRef> BuildRuntimeDependencies(
        const NLS::Core::Assets::ArtifactManifest& manifest)
    {
        std::vector<NLS::Engine::Assets::RuntimeAssetRef> dependencies;
        for (const auto& dependency : manifest.dependencies)
        {
            auto reference = ToRuntimeDependencyRef(dependency);
            if (reference.has_value() &&
                std::find(dependencies.begin(), dependencies.end(), *reference) == dependencies.end())
            {
                dependencies.push_back(*reference);
            }
        }
        return dependencies;
    }

    bool ContainsPackSelection(
        const std::vector<std::pair<std::string, std::string>>& packs,
        const NLS::Engine::Assets::RuntimeAssetPack& pack)
    {
        return std::any_of(
            packs.begin(),
            packs.end(),
            [&pack](const auto& selection)
            {
                return selection.first == pack.packName && selection.second == pack.packVariant;
            });
    }

    void AddUniqueReference(
        std::vector<NLS::Engine::Assets::RuntimeAssetRef>& references,
        const NLS::Engine::Assets::RuntimeAssetRef& reference)
    {
        if (std::find(references.begin(), references.end(), reference) == references.end())
            references.push_back(reference);
    }

    std::vector<NLS::Engine::Assets::RuntimeAssetRef> ResolvePrewarmRoots(
        const NLS::Engine::Assets::RuntimeAssetManifest& manifest,
        const RuntimeMaterialPrewarmOptions& options)
    {
        std::vector<NLS::Engine::Assets::RuntimeAssetRef> roots = options.roots;

        for (const auto& pack : manifest.assetPacks)
        {
            if (!ContainsPackSelection(options.assetPacks, pack))
                continue;

            for (const auto& entry : pack.entries)
                AddUniqueReference(roots, entry.reference);
        }

        if (roots.empty() && options.assetPacks.empty())
            roots = manifest.roots;

        return roots;
    }

    NLS::Engine::Assets::RuntimeAssetManifestEntry MakeRuntimeEntry(
        const NLS::Core::Assets::ArtifactDatabaseRecord& record,
        std::vector<NLS::Engine::Assets::RuntimeAssetRef> dependencies)
    {
        return {
            record.sourceAssetId,
            record.subAssetKey,
            record.artifactType,
            record.loaderId,
            record.artifactPath,
            record.contentHash,
            std::move(dependencies)
        };
    }
}

std::filesystem::path ResolveRuntimeArtifactDatabasePathForProjectSettings(
    const std::filesystem::path& projectSettingsPath)
{
    if (!projectSettingsPath.empty())
    {
        const auto projectRoot = projectSettingsPath.parent_path();
        if (!projectRoot.empty())
            return projectRoot / "Library" / "ArtifactDB";
    }

    return std::filesystem::current_path() / "Data" / "ArtifactDB";
}

std::optional<NLS::Engine::Assets::RuntimeAssetDatabase> LoadRuntimeAssetDatabaseForProjectSettings(
    const std::filesystem::path& projectSettingsPath)
{
    NLS::Core::Assets::ClearRuntimeArtifactAuthorization();
    NLS::Core::Assets::SetRuntimeArtifactAuthorizationEnabled(false);

    NLS::Core::Assets::ArtifactDatabase artifactDatabase;
    if (!artifactDatabase.Load(ResolveRuntimeArtifactDatabasePathForProjectSettings(projectSettingsPath)))
        return std::nullopt;

    NLS::Engine::Assets::RuntimeAssetManifest manifest;
    manifest.schemaVersion = 1u;

    const auto stats = artifactDatabase.GetStats();
    if (stats.totalRecords == 0u)
        return std::nullopt;

    std::vector<const NLS::Core::Assets::ArtifactDatabaseRecord*> records;
    records.reserve(stats.totalRecords);
    bool hasInvalidRecord = false;
    std::string targetPlatform;
    artifactDatabase.VisitRecords([&](const NLS::Core::Assets::ArtifactDatabaseRecord& record)
    {
        if (record.status != NLS::Core::Assets::ArtifactRecordStatus::UpToDate)
            return;
        if (record.targetPlatform.empty())
        {
            hasInvalidRecord = true;
            return;
        }
        if (targetPlatform.empty())
            targetPlatform = record.targetPlatform;
        else if (targetPlatform != record.targetPlatform)
        {
            hasInvalidRecord = true;
            return;
        }
        if (record.targetPlatform != kRuntimeTargetPlatform)
        {
            hasInvalidRecord = true;
            return;
        }
        if (!IsValidRuntimeArtifactEntry(
            record.artifactType,
            record.loaderId,
            record.artifactPath,
            record.contentHash))
        {
            hasInvalidRecord = true;
            return;
        }
        records.push_back(&record);
    });

    if (hasInvalidRecord || records.empty())
        return std::nullopt;

    for (const auto* record : records)
    {
        if (record == nullptr)
            return std::nullopt;

        if (manifest.targetPlatform.empty())
            manifest.targetPlatform = record->targetPlatform;
        const auto sourceManifest = artifactDatabase.BuildManifestForSource(record->sourceAssetId, record->targetPlatform);
        if (!sourceManifest.has_value())
            return std::nullopt;

        manifest.entries.push_back(MakeRuntimeEntry(*record, BuildRuntimeDependencies(*sourceManifest)));
        if (record->primarySubAssetKey == record->subAssetKey)
            AddUniqueReference(manifest.roots, {record->sourceAssetId, record->subAssetKey});
    }

    RegisterRuntimeManifestArtifactAuthorizations(manifest);
    return NLS::Engine::Assets::RuntimeAssetDatabase(std::move(manifest));
}

size_t PrewarmRuntimeMaterialAssets(
    const NLS::Engine::Assets::RuntimeAssetDatabase& runtimeDatabase,
    NLS::Core::ResourceManagement::MaterialManager& materialManager,
    const RuntimeMaterialPrewarmOptions& options)
{
    size_t loadedCount = 0u;
    std::queue<NLS::Engine::Assets::RuntimeAssetRef> pending;
    std::unordered_set<NLS::Engine::Assets::RuntimeAssetRef> visited;

    for (const auto& root : ResolvePrewarmRoots(runtimeDatabase.GetManifest(), options))
        pending.push(root);

    while (!pending.empty())
    {
        const auto reference = pending.front();
        pending.pop();

        if (!visited.insert(reference).second)
            continue;

        const auto* entry = runtimeDatabase.Resolve(reference);
        if (entry == nullptr)
            continue;

        for (const auto& dependency : entry->dependencies)
            pending.push(dependency);

        if (entry->artifactPath.empty() || !IsMaterialEntry(entry->artifactType, entry->loaderId))
            continue;

        if (materialManager.LoadArtifactWithoutTextures(entry->artifactPath) != nullptr)
            ++loadedCount;
    }

    return loadedCount;
}
}
