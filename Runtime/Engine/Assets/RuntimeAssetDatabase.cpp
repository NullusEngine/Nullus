#include "Engine/Assets/RuntimeAssetDatabase.h"
#include "Rendering/Assets/DistanceFieldArtifact.h"
#include "Rendering/Assets/SceneImportPipeline.h"

#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/Resources/Loaders/ShaderPassArtifactRegistry.h"
#include "Serialize/ObjectGraphDocument.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <queue>
#include <map>
#include <unordered_set>
#include <utility>

namespace NLS::Engine::Assets
{
namespace
{
std::string NormalizeExtension(std::filesystem::path path)
{
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return extension;
}

bool HasWindowsRootNameSyntax(const std::string& path)
{
    if (path.size() < 2u)
        return false;

    const auto first = static_cast<unsigned char>(path[0]);
    return std::isalpha(first) && path[1] == ':';
}

bool IsEditorHostTexturePlatform(const NLS::Core::Assets::ArtifactType type, const std::string& platform, const std::string& target)
{
    // Standalone texture imports contain native compression and use a concrete
    // host target. Editor programs/materials are portable and use "editor".
    // Permit only the current editor's texture target in that dependency closure;
    // packaged platform builds still require an exact target match.
#if defined(_WIN32)
    return target == "editor" && type == NLS::Core::Assets::ArtifactType::Texture && platform == "win64-dx12";
#else
    return false;
#endif
}

bool ContainsEntry(const RuntimeAssetManifest& manifest, const RuntimeAssetRef& reference)
{
    return std::any_of(
        manifest.entries.begin(),
        manifest.entries.end(),
        [&reference](const RuntimeAssetManifestEntry& entry)
        {
            return entry.assetId == reference.assetId && entry.subAssetKey == reference.subAssetKey;
        });
}

RuntimeAssetRef MakeReference(
    NLS::Core::Assets::AssetId sourceAssetId,
    const NLS::Core::Assets::ImportedArtifact& artifact)
{
    return {
        artifact.sourceAssetId.IsValid() ? artifact.sourceAssetId : sourceAssetId,
        artifact.subAssetKey
    };
}

RuntimeAssetManifestEntry MakeEntry(
    NLS::Core::Assets::AssetId sourceAssetId,
    const NLS::Core::Assets::ImportedArtifact& artifact)
{
    return {
        artifact.sourceAssetId.IsValid() ? artifact.sourceAssetId : sourceAssetId,
        artifact.subAssetKey,
        artifact.artifactType,
        artifact.loaderId,
        artifact.artifactPath,
        artifact.contentHash,
        {}
    };
}

RuntimeAssetPackEntry MakePackEntry(
    NLS::Core::Assets::AssetId sourceAssetId,
    const NLS::Core::Assets::ImportedArtifact& artifact)
{
    return {
        MakeReference(sourceAssetId, artifact),
        artifact.artifactType,
        artifact.loaderId,
        artifact.artifactPath,
        artifact.contentHash,
        {}
    };
}

bool ContainsPackEntry(const std::vector<RuntimeAssetPackEntry>& entries, const RuntimeAssetRef& reference)
{
    return std::any_of(
        entries.begin(),
        entries.end(),
        [&reference](const RuntimeAssetPackEntry& entry)
        {
            return entry.reference == reference;
        });
}

RuntimeAssetManifestEntry* FindEntry(RuntimeAssetManifest& manifest, const RuntimeAssetRef& reference)
{
    const auto found = std::find_if(
        manifest.entries.begin(),
        manifest.entries.end(),
        [&reference](const RuntimeAssetManifestEntry& entry)
        {
            return entry.assetId == reference.assetId && entry.subAssetKey == reference.subAssetKey;
        });
    return found != manifest.entries.end() ? &(*found) : nullptr;
}

RuntimeAssetPackEntry* FindPackEntry(std::vector<RuntimeAssetPackEntry>& entries, const RuntimeAssetRef& reference)
{
    const auto found = std::find_if(
        entries.begin(),
        entries.end(),
        [&reference](const RuntimeAssetPackEntry& entry)
        {
            return entry.reference == reference;
        });
    return found != entries.end() ? &(*found) : nullptr;
}

void AddUniqueDependency(std::vector<RuntimeAssetRef>& dependencies, const RuntimeAssetRef& dependency)
{
    if (std::find(dependencies.begin(), dependencies.end(), dependency) == dependencies.end())
        dependencies.push_back(dependency);
}

bool IsBuildDependencyKind(NLS::Core::Assets::AssetDependencyKind kind)
{
    return kind == NLS::Core::Assets::AssetDependencyKind::ImportedArtifact ||
        kind == NLS::Core::Assets::AssetDependencyKind::PrefabBase ||
        kind == NLS::Core::Assets::AssetDependencyKind::NestedPrefab;
}

std::optional<RuntimeAssetRef> ToRuntimeDependencyRef(const NLS::Core::Assets::AssetDependencyRecord& dependency)
{
    if (!IsBuildDependencyKind(dependency.kind))
        return std::nullopt;

    const auto parsedGuid = NLS::Guid::TryParse(dependency.value);
    if (!parsedGuid.has_value())
        return std::nullopt;

    return RuntimeAssetRef {
        NLS::Core::Assets::AssetId(*parsedGuid),
        dependency.hashOrVersion
    };
}

void SortReferences(std::vector<RuntimeAssetRef>& references)
{
    std::sort(
        references.begin(),
        references.end(),
        [](const RuntimeAssetRef& lhs, const RuntimeAssetRef& rhs)
        {
            if (lhs.assetId.ToString() != rhs.assetId.ToString())
                return lhs.assetId.ToString() < rhs.assetId.ToString();
            return lhs.subAssetKey < rhs.subAssetKey;
        });
}

void SortPackEntries(std::vector<RuntimeAssetPackEntry>& entries)
{
    std::sort(
        entries.begin(),
        entries.end(),
        [](const RuntimeAssetPackEntry& lhs, const RuntimeAssetPackEntry& rhs)
        {
            if (lhs.reference.assetId.ToString() != rhs.reference.assetId.ToString())
                return lhs.reference.assetId.ToString() < rhs.reference.assetId.ToString();
            return lhs.reference.subAssetKey < rhs.reference.subAssetKey;
        });
}

bool ValidateRuntimeArtifact(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const NLS::Core::Assets::ImportedArtifact& artifact,
    const std::string& targetPlatform,
    NLS::Core::Assets::ImportDiagnosticList& diagnostics)
{
    if (!artifact.targetPlatform.empty() && artifact.targetPlatform != targetPlatform &&
        !IsEditorHostTexturePlatform(artifact.artifactType,artifact.targetPlatform,targetPlatform))
    {
        diagnostics.Add({
            NLS::Core::Assets::ImportDiagnosticSeverity::Error,
            "runtime-manifest-artifact-platform-mismatch",
            manifest.sourceAssetId,
            artifact.subAssetKey,
            artifact.targetPlatform,
            true
        });
        return false;
    }

    if (!IsRuntimePackagedAssetPath(artifact.artifactPath))
    {
        diagnostics.Add({
            NLS::Core::Assets::ImportDiagnosticSeverity::Error,
            "runtime-manifest-source-artifact-path",
            manifest.sourceAssetId,
            artifact.subAssetKey,
            artifact.artifactPath,
            true
        });
        return false;
    }

    if (artifact.loaderId.empty() || artifact.contentHash.empty())
    {
        diagnostics.Add({
            NLS::Core::Assets::ImportDiagnosticSeverity::Error,
            "runtime-manifest-incomplete-artifact-metadata",
            manifest.sourceAssetId,
            artifact.subAssetKey,
            artifact.artifactPath,
            true
        });
        return false;
    }

    return true;
}

void AddDistanceFieldRuntimeDependencies(const NLS::Core::Assets::ArtifactManifest& manifest,
    const NLS::Core::Assets::ImportedArtifact& artifact, std::vector<RuntimeAssetRef>& dependencies,
    std::queue<RuntimeAssetRef>& pending)
{
    std::vector<std::string> keys;
    if (artifact.artifactType == NLS::Core::Assets::ArtifactType::Mesh)
    {
        keys.push_back(NLS::Render::DistanceField::DistanceFieldSubAssetKey(artifact.subAssetKey));
        if (artifact.subAssetKey.starts_with("mesh:"))
            if (const auto primitive=NLS::Render::Assets::ParsePrimitiveMeshSourceKey(artifact.subAssetKey.substr(5)))
            {
                const auto groupKey=NLS::Render::DistanceField::DistanceFieldSubAssetKey("mesh:"+primitive->first);
                if (manifest.FindSubAsset(groupKey))
                {
                    keys.push_back(groupKey);
                    // Keep the membership complete even for a mesh-only pack. A
                    // renderer containing just one part must use its own field.
                    for (const auto& member:manifest.subAssets)
                        if (member.artifactType==NLS::Core::Assets::ArtifactType::Mesh && member.subAssetKey!=artifact.subAssetKey && member.subAssetKey.starts_with("mesh:"))
                            if (const auto other=NLS::Render::Assets::ParsePrimitiveMeshSourceKey(member.subAssetKey.substr(5));other && other->first==primitive->first)
                            {
                                RuntimeAssetRef reference{manifest.sourceAssetId,member.subAssetKey};
                                AddUniqueDependency(dependencies,reference); pending.push(std::move(reference));
                            }
                }
            }
    }
    else if (artifact.artifactType == NLS::Core::Assets::ArtifactType::MeshDistanceField && artifact.loaderId == "mesh-distance-field")
        for (uint32_t mip = 0; mip < 2; ++mip) keys.push_back(artifact.subAssetKey + ":mip" + std::to_string(mip));
    for (const auto& key : keys)
        if (manifest.FindSubAsset(key))
        {
            RuntimeAssetRef reference{manifest.sourceAssetId, key};
            AddUniqueDependency(dependencies, reference);
            pending.push(std::move(reference));
        }
}

bool ShouldIncludeSiblingArtifactsForRoot(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const RuntimeAssetRef& root,
    const NLS::Core::Assets::ImportedArtifact& rootArtifact)
{
    if (root.subAssetKey == manifest.primarySubAssetKey)
        return true;

    return rootArtifact.artifactType == NLS::Core::Assets::ArtifactType::Prefab ||
        rootArtifact.artifactType == NLS::Core::Assets::ArtifactType::Model;
}
}

RuntimeAssetDatabase::RuntimeAssetDatabase(RuntimeAssetManifest manifest)
    : m_manifest(std::move(manifest))
{
    RebuildEntryIndex();

    // Packaged materials address shaders by source path. Register the pass
    // artifacts from the manifest so MaterialLoader can bind them without an
    // ArtifactDB (which is a development-only database).
    std::map<std::string, std::vector<std::string>> shaderPasses;
    for (const auto& entry : m_manifest.entries)
    {
        if (entry.artifactType != NLS::Core::Assets::ArtifactType::Shader ||
            entry.artifactPath.empty())
        {
            continue;
        }
        const auto artifact = NLS::Render::Assets::LoadShaderArtifact(entry.artifactPath);
        if (!artifact.has_value() || artifact->sourcePath.empty())
            continue;
        shaderPasses[artifact->sourcePath].push_back(entry.artifactPath);
    }
    for (auto& [sourcePath, artifacts] : shaderPasses)
    {
        NLS::Render::Resources::Loaders::RegisterShaderPassArtifacts(
            sourcePath,
            std::move(artifacts));
    }
}

bool IsRuntimeAssetPlatformCompatible(Core::Assets::ArtifactType type, const std::string& platform, const std::string& target)
{
    return platform == target || IsEditorHostTexturePlatform(type,platform,target);
}

const RuntimeAssetManifestEntry* RuntimeAssetDatabase::Resolve(
    NLS::Core::Assets::AssetId assetId,
    const std::string& subAssetKey) const
{
    return Resolve(RuntimeAssetRef {assetId, subAssetKey});
}

const RuntimeAssetManifestEntry* RuntimeAssetDatabase::Resolve(const RuntimeAssetRef& reference) const
{
    const auto found = m_entryIndex.find(reference);
    if (found != m_entryIndex.end() && found->second < m_manifest.entries.size())
        return &m_manifest.entries[found->second];
    return nullptr;
}

const RuntimeAssetManifestEntry* RuntimeAssetDatabase::ResolvePrimary(
    NLS::Core::Assets::AssetId assetId) const
{
    if (!assetId.IsValid())
        return nullptr;

    const RuntimeAssetManifestEntry* first = nullptr;
    for (const auto& entry : m_manifest.entries)
    {
        if (entry.assetId != assetId)
            continue;
        if (entry.subAssetKey.empty())
            return &entry;
        if (first == nullptr)
            first = &entry;
    }
    return first;
}

const RuntimeAssetManifestEntry* RuntimeAssetDatabase::ResolveByLocalIdentifierInFile(
    NLS::Core::Assets::AssetId assetId,
    int64_t localIdentifierInFile) const
{
    const auto found = m_localIdentifierIndex.find({assetId, localIdentifierInFile});
    if (found != m_localIdentifierIndex.end() && found->second < m_manifest.entries.size())
        return &m_manifest.entries[found->second];
    return nullptr;
}

const RuntimeAssetManifest& RuntimeAssetDatabase::GetManifest() const
{
    return m_manifest;
}

size_t RuntimeAssetDatabase::GetIndexedEntryCount() const
{
    return m_entryIndex.size();
}

void RuntimeAssetDatabase::RebuildEntryIndex()
{
    m_entryIndex.clear();
    m_localIdentifierIndex.clear();
    m_entryIndex.reserve(m_manifest.entries.size());
    m_localIdentifierIndex.reserve(m_manifest.entries.size());
    for (size_t index = 0u; index < m_manifest.entries.size(); ++index)
    {
        const auto& entry = m_manifest.entries[index];
        m_entryIndex[{entry.assetId, entry.subAssetKey}] = index;
        m_localIdentifierIndex[{
            entry.assetId,
            Serialize::MakeLocalIdentifierInFile(entry.assetId.GetGuid(), entry.subAssetKey)
        }] = index;
    }
    std::map<std::string, NLS::Core::Assets::ArtifactManifest> distanceFieldManifests;
    for (const auto& entry : m_manifest.entries)
    {
        auto& manifest = distanceFieldManifests[entry.assetId.ToString()];
        manifest.sourceAssetId = entry.assetId;
        manifest.targetPlatform = m_manifest.targetPlatform;
        NLS::Core::Assets::ImportedArtifact artifact;
        artifact.sourceAssetId = entry.assetId;
        artifact.subAssetKey = entry.subAssetKey;
        artifact.artifactType = entry.artifactType;
        artifact.loaderId = entry.loaderId;
        artifact.artifactPath = entry.artifactPath;
        artifact.contentHash = entry.contentHash;
        manifest.subAssets.push_back(std::move(artifact));
    }
    for (const auto& [key, manifest] : distanceFieldManifests)
        NLS::Render::DistanceField::RegisterDistanceFieldManifest(manifest);
}

void RuntimeManifestBuilder::AddArtifactManifest(NLS::Core::Assets::ArtifactManifest manifest)
{
    m_manifests.push_back(std::move(manifest));
}

const NLS::Core::Assets::ArtifactManifest* RuntimeManifestBuilder::FindManifest(
    NLS::Core::Assets::AssetId assetId,
    const std::string& targetPlatform) const
{
    for (const auto& manifest : m_manifests)
    {
        const bool hostTextures = !manifest.subAssets.empty() && std::all_of(manifest.subAssets.begin(),manifest.subAssets.end(),[&](const auto& artifact)
        { return IsEditorHostTexturePlatform(artifact.artifactType,manifest.targetPlatform,targetPlatform); });
        if (manifest.sourceAssetId == assetId && (manifest.targetPlatform == targetPlatform || hostTextures))
            return &manifest;
        if (manifest.targetPlatform != targetPlatform && !hostTextures)
            continue;
        const auto found = std::find_if(
            manifest.subAssets.begin(),
            manifest.subAssets.end(),
            [assetId](const NLS::Core::Assets::ImportedArtifact& artifact)
            {
                return artifact.sourceAssetId == assetId;
            });
        if (found != manifest.subAssets.end())
            return &manifest;
    }
    return nullptr;
}

RuntimeManifestBuildResult RuntimeManifestBuilder::Build(
    const std::vector<RuntimeAssetRef>& roots,
    std::string targetPlatform) const
{
    RuntimeManifestBuildResult result;
    result.manifest.schemaVersion = 1u;
    result.manifest.targetPlatform = targetPlatform;
    result.manifest.roots.clear();

    std::queue<RuntimeAssetRef> pending;
    std::unordered_set<RuntimeAssetRef> visited;
    for (auto root : roots)
    {
        // Callers may reference an asset by GUID alone. Normalise that to the
        // manifest's primary sub-asset so lookups and reasons stay consistent.
        if (root.subAssetKey.empty())
        {
            if (const auto* manifest = FindManifest(root.assetId, targetPlatform))
                root.subAssetKey = manifest->primarySubAssetKey;
        }
        result.manifest.roots.push_back(root);
        pending.push(root);
    }

    while (!pending.empty())
    {
        auto current = pending.front();
        pending.pop();

        if (!visited.insert(current).second)
            continue;

        const auto* sourceManifest = FindManifest(current.assetId, targetPlatform);
        if (!sourceManifest)
        {
            result.diagnostics.Add({
                NLS::Core::Assets::ImportDiagnosticSeverity::Error,
                "runtime-manifest-missing-artifact-manifest",
                current.assetId,
                current.subAssetKey,
                "",
                true
            });
            continue;
        }

        if (current.subAssetKey.empty())
            current.subAssetKey = sourceManifest->primarySubAssetKey;
        if (current.subAssetKey.empty())
        {
            result.diagnostics.Add({
                NLS::Core::Assets::ImportDiagnosticSeverity::Error,
                "runtime-manifest-missing-root-subasset",
                current.assetId,
                current.subAssetKey,
                "",
                true
            });
            continue;
        }
        visited.insert(current);

        const auto* currentArtifact = sourceManifest->FindSubAsset(current.subAssetKey);
        if (!currentArtifact)
        {
            result.diagnostics.Add({
                NLS::Core::Assets::ImportDiagnosticSeverity::Error,
                "runtime-manifest-missing-root-subasset",
                current.assetId,
                current.subAssetKey,
                "",
                true
            });
            continue;
        }

        std::vector<RuntimeAssetRef> currentDependencies;
        for (const auto& dependency : sourceManifest->dependencies)
        {
            auto dependencyRef = ToRuntimeDependencyRef(dependency);
            if (!dependencyRef.has_value())
                continue;

            AddUniqueDependency(currentDependencies, *dependencyRef);
            pending.push(*dependencyRef);
        }

        AddDistanceFieldRuntimeDependencies(*sourceManifest, *currentArtifact, currentDependencies, pending);
        const auto addArtifact = [&](const NLS::Core::Assets::ImportedArtifact& artifact)
        {
            if (!ValidateRuntimeArtifact(
                *sourceManifest,
                artifact,
                result.manifest.targetPlatform,
                result.diagnostics))
            {
                return;
            }

            const auto reference = MakeReference(sourceManifest->sourceAssetId, artifact);
            if (!ContainsEntry(result.manifest, reference))
                result.manifest.entries.push_back(MakeEntry(sourceManifest->sourceAssetId, artifact));

            if (auto* entry = FindEntry(result.manifest, reference))
                entry->dependencies = currentDependencies;

            if (artifact.artifactType == NLS::Core::Assets::ArtifactType::Prefab)
            {
                result.manifest.prefabEntries.push_back({
                    reference,
                    artifact.artifactPath,
                    currentDependencies
                });
            }
        };

        addArtifact(*currentArtifact);

        if (ShouldIncludeSiblingArtifactsForRoot(*sourceManifest, current, *currentArtifact))
        {
            for (const auto& artifact : sourceManifest->subAssets)
            {
                if (artifact.subAssetKey == current.subAssetKey)
                    continue;
                addArtifact(artifact);
            }
        }
    }

    return result;
}

RuntimeManifestBuildResult RuntimeManifestBuilder::BuildAssetPacks(
    const std::vector<AssetPackBuildInput>& packs,
    std::string targetPlatform) const
{
    std::vector<RuntimeAssetRef> roots;
    roots.reserve(packs.size());
    for (const auto& pack : packs)
        roots.push_back(pack.root);

    auto result = Build(roots, std::move(targetPlatform));

    std::map<std::pair<std::string, std::string>, std::vector<RuntimeAssetRef>> groupedPacks;
    for (const auto& pack : packs)
        groupedPacks[{pack.packName, pack.packVariant}].push_back(pack.root);

    for (auto& [bundle, roots] : groupedPacks)
    {
        std::queue<RuntimeAssetRef> pending;
        std::unordered_set<RuntimeAssetRef> visited;
        std::vector<RuntimeAssetPackEntry> packEntries;

        SortReferences(roots);
        roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
        for (const auto& root : roots)
            pending.push(root);

        while (!pending.empty())
        {
            auto current = pending.front();
            pending.pop();

            if (!visited.insert(current).second)
                continue;

            const auto* sourceManifest = FindManifest(current.assetId, result.manifest.targetPlatform);
            if (!sourceManifest)
                continue;

            if (current.subAssetKey.empty())
                current.subAssetKey = sourceManifest->primarySubAssetKey;
            if (current.subAssetKey.empty())
                continue;
            visited.insert(current);

            const auto* currentArtifact = sourceManifest->FindSubAsset(current.subAssetKey);
            if (!currentArtifact)
                continue;

            std::vector<RuntimeAssetRef> dependencies;
            for (const auto& dependency : sourceManifest->dependencies)
            {
                auto dependencyRef = ToRuntimeDependencyRef(dependency);
                if (!dependencyRef.has_value())
                    continue;

                AddUniqueDependency(dependencies, *dependencyRef);
                pending.push(*dependencyRef);
            }

            AddDistanceFieldRuntimeDependencies(*sourceManifest, *currentArtifact, dependencies, pending);
            const auto addPackArtifact = [&](const NLS::Core::Assets::ImportedArtifact& artifact)
            {
                if (!ValidateRuntimeArtifact(
                    *sourceManifest,
                    artifact,
                    result.manifest.targetPlatform,
                    result.diagnostics))
                {
                    return;
                }

                const auto reference = MakeReference(sourceManifest->sourceAssetId, artifact);
                if (!ContainsPackEntry(packEntries, reference))
                    packEntries.push_back(MakePackEntry(sourceManifest->sourceAssetId, artifact));

                if (auto* entry = FindPackEntry(packEntries, reference))
                    entry->dependencies = dependencies;
            };

            addPackArtifact(*currentArtifact);

            if (ShouldIncludeSiblingArtifactsForRoot(*sourceManifest, current, *currentArtifact))
            {
                for (const auto& artifact : sourceManifest->subAssets)
                {
                    if (artifact.subAssetKey == current.subAssetKey)
                        continue;
                    addPackArtifact(artifact);
                }
            }
        }

        for (auto& entry : packEntries)
        {
            SortReferences(entry.dependencies);
            entry.dependencies.erase(
                std::unique(entry.dependencies.begin(), entry.dependencies.end()),
                entry.dependencies.end());
        }
        SortPackEntries(packEntries);

        result.manifest.assetPacks.push_back({
            bundle.first,
            bundle.second,
            std::move(packEntries)
        });
    }

    return result;
}

bool IsRuntimePackagedAssetPath(const std::string& path)
{
    if (path.empty())
        return false;

    if (HasWindowsRootNameSyntax(path))
        return false;

    const auto rawPath = std::filesystem::path(path);
    if (rawPath.is_absolute())
        return false;

    if (rawPath.has_root_name() || rawPath.has_root_directory())
        return false;

    for (const auto& part : rawPath)
    {
        if (part == "..")
            return false;
    }

    const auto extension = NormalizeExtension(path);
    if (extension == ".gltf" ||
        extension == ".glb" ||
        extension == ".fbx" ||
        extension == ".obj" ||
        extension == ".prefab" ||
        extension == ".mat" ||
        extension == ".png" ||
        extension == ".jpg" ||
        extension == ".jpeg" ||
        extension == ".tga" ||
        extension == ".scene" ||
        extension == ".ui" ||
        extension == ".style" ||
        extension == ".loc" ||
        extension == ".meta")
    {
        return false;
    }

    const auto normalized = std::filesystem::path(path).generic_string();
    if (!NLS::Core::Assets::IsContentStorageArtifactPath(normalized))
        return false;

    if (normalized.find("SourceAssetDatabase") != std::string::npos)
        return false;

    if (normalized.rfind("Assets/", 0u) == 0u ||
        normalized.rfind("EngineAssets/", 0u) == 0u ||
        normalized.rfind("Packages/", 0u) == 0u ||
        normalized.rfind("Library/SourceAssetDatabase", 0u) == 0u ||
        normalized.rfind("Library/Editor", 0u) == 0u)
    {
        return false;
    }

    return true;
}

bool IsRuntimeAssetApiAvailable(const std::string& apiName)
{
    static constexpr const char* kEditorOnlyPrefixes[] = {
        "AssetDatabase.",
        "AssetImporter.",
        "ModelImporter.",
        "TextureImporter.",
        "PrefabImporter.",
        "AssetPostprocessor."
    };

    for (const auto* prefix : kEditorOnlyPrefixes)
    {
        if (apiName.rfind(prefix, 0u) == 0u)
            return false;
    }

    return true;
}
}
