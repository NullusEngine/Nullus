#pragma once

#include "Assets/ArtifactManifest.h"
#include "Assets/ImportDiagnostics.h"
#include "EngineDef.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace NLS::Engine::Assets
{
struct RuntimeAssetRef
{
    NLS::Core::Assets::AssetId assetId;
    std::string subAssetKey;

    friend bool operator==(const RuntimeAssetRef& lhs, const RuntimeAssetRef& rhs) = default;
};

struct RuntimeAssetRefHash
{
    size_t operator()(const RuntimeAssetRef& reference) const noexcept
    {
        size_t hash = std::hash<NLS::Core::Assets::AssetId>{}(reference.assetId);
        hash ^= std::hash<std::string>{}(reference.subAssetKey) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        return hash;
    }
};

struct RuntimeAssetLocalIdentifier
{
    NLS::Core::Assets::AssetId assetId;
    int64_t localIdentifierInFile = 0;

    friend bool operator==(const RuntimeAssetLocalIdentifier& lhs, const RuntimeAssetLocalIdentifier& rhs) = default;
};

struct RuntimeAssetLocalIdentifierHash
{
    size_t operator()(const RuntimeAssetLocalIdentifier& reference) const noexcept
    {
        size_t hash = std::hash<NLS::Core::Assets::AssetId>{}(reference.assetId);
        hash ^= std::hash<int64_t>{}(reference.localIdentifierInFile) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        return hash;
    }
};

struct RuntimeAssetManifestEntry
{
    NLS::Core::Assets::AssetId assetId;
    std::string subAssetKey;
    NLS::Core::Assets::ArtifactType artifactType = NLS::Core::Assets::ArtifactType::Unknown;
    std::string loaderId;
    std::string artifactPath;
    std::string contentHash;
    std::vector<RuntimeAssetRef> dependencies;
};

struct RuntimePrefabManifestEntry
{
    RuntimeAssetRef prefab;
    std::string graphArtifactPath;
    std::vector<RuntimeAssetRef> dependencies;
};

struct RuntimeAssetPackEntry
{
    RuntimeAssetRef reference;
    NLS::Core::Assets::ArtifactType artifactType = NLS::Core::Assets::ArtifactType::Unknown;
    std::string loaderId;
    std::string artifactPath;
    std::string contentHash;
    std::vector<RuntimeAssetRef> dependencies;
};

struct RuntimeAssetPack
{
    std::string packName;
    std::string packVariant;
    std::vector<RuntimeAssetPackEntry> entries;
};

struct NLS_ENGINE_API RuntimeAssetManifest
{
    uint32_t schemaVersion = 1u;
    std::string targetPlatform;
    std::vector<RuntimeAssetRef> roots;
    std::vector<RuntimeAssetManifestEntry> entries;
    std::vector<RuntimePrefabManifestEntry> prefabEntries;
    std::vector<RuntimeAssetPack> assetPacks;
};

struct AssetPackBuildInput
{
    std::string packName;
    std::string packVariant;
    RuntimeAssetRef root;
};

class NLS_ENGINE_API RuntimeAssetDatabase
{
public:
    RuntimeAssetDatabase() = default;
    explicit RuntimeAssetDatabase(RuntimeAssetManifest manifest);

    const RuntimeAssetManifestEntry* Resolve(
        NLS::Core::Assets::AssetId assetId,
        const std::string& subAssetKey) const;
    const RuntimeAssetManifestEntry* Resolve(const RuntimeAssetRef& reference) const;
    const RuntimeAssetManifestEntry* ResolveByLocalIdentifierInFile(
        NLS::Core::Assets::AssetId assetId,
        int64_t localIdentifierInFile) const;

    const RuntimeAssetManifest& GetManifest() const;
    size_t GetIndexedEntryCount() const;

private:
    void RebuildEntryIndex();

    RuntimeAssetManifest m_manifest;
    std::unordered_map<RuntimeAssetRef, size_t, RuntimeAssetRefHash> m_entryIndex;
    std::unordered_map<RuntimeAssetLocalIdentifier, size_t, RuntimeAssetLocalIdentifierHash> m_localIdentifierIndex;
};

struct RuntimeManifestBuildResult
{
    RuntimeAssetManifest manifest;
    NLS::Core::Assets::ImportDiagnosticList diagnostics;
};

class NLS_ENGINE_API RuntimeManifestBuilder
{
public:
    void AddArtifactManifest(NLS::Core::Assets::ArtifactManifest manifest);
    RuntimeManifestBuildResult Build(
        const std::vector<RuntimeAssetRef>& roots,
        std::string targetPlatform) const;
    RuntimeManifestBuildResult BuildAssetPacks(
        const std::vector<AssetPackBuildInput>& packs,
        std::string targetPlatform) const;

private:
    const NLS::Core::Assets::ArtifactManifest* FindManifest(
        NLS::Core::Assets::AssetId assetId,
        const std::string& targetPlatform) const;

    std::vector<NLS::Core::Assets::ArtifactManifest> m_manifests;
};

NLS_ENGINE_API bool IsRuntimePackagedAssetPath(const std::string& path);
NLS_ENGINE_API bool IsRuntimeAssetApiAvailable(const std::string& apiName);
}

namespace std
{
template<>
struct hash<NLS::Engine::Assets::RuntimeAssetRef>
{
    size_t operator()(const NLS::Engine::Assets::RuntimeAssetRef& reference) const noexcept
    {
        return NLS::Engine::Assets::RuntimeAssetRefHash{}(reference);
    }
};

template<>
struct hash<NLS::Engine::Assets::RuntimeAssetLocalIdentifier>
{
    size_t operator()(const NLS::Engine::Assets::RuntimeAssetLocalIdentifier& reference) const noexcept
    {
        return NLS::Engine::Assets::RuntimeAssetLocalIdentifierHash{}(reference);
    }
};
}
