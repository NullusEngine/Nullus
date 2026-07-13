#include "Engine/Assets/PrefabAsset.h"

#include "Assets/ArtifactManifest.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ServiceLocator.h"
#include "Profiling/PerformanceStageStats.h"
#include "Serialize/ObjectGraphInstantiator.h"
#include "Serialize/ObjectGraphReader.h"
#include "Serialize/PrefabDocument.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NLS::Engine::Assets
{
namespace
{
struct PrefabAssetReference
{
    Serialize::ObjectIdentifier reference;
    std::string expectedType;
};

struct PrefabResolvedAssetValidationIndex
{
    std::unordered_set<std::string> assetIdsWithResolvedAssets;
    std::unordered_set<std::string> referenceKeys;
};

struct PrefabResolvedAssetValidationStats
{
    size_t indexKeyCount = 0u;
    size_t lookupCount = 0u;
};

void AddDiagnostic(
    Serialize::SerializationDiagnosticList& diagnostics,
    Serialize::SerializationDiagnosticCode code,
    Serialize::SerializationDiagnosticSeverity severity,
    std::string message)
{
    diagnostics.Add({code, severity, std::move(message)});
}

const Serialize::ObjectRecord* FindObjectRecord(
    const Serialize::ObjectGraphDocument& graph,
    const Serialize::ObjectId& id)
{
    for (const auto& record : graph.objects)
    {
        if (record.id == id)
            return &record;
    }
    return nullptr;
}

const Serialize::PropertyRecord* FindProperty(
    const Serialize::ObjectRecord& record,
    const std::string& name)
{
    for (const auto& property : record.properties)
    {
        if (property.name == name)
            return &property;
    }
    return nullptr;
}

bool ResolvedAssetReferencePathMatches(
    const PrefabResolvedAsset& resolved,
    const std::string& referenceFilePath)
{
    if (referenceFilePath.empty())
        return resolved.subAssetKey.empty() && resolved.artifactPath.empty();

    if (resolved.subAssetKey == referenceFilePath ||
        resolved.artifactPath == referenceFilePath)
    {
        return true;
    }

    const auto referencePath = std::filesystem::path(referenceFilePath).lexically_normal();
    const auto resolvedPath = std::filesystem::path(resolved.artifactPath).lexically_normal();
    return !resolved.artifactPath.empty() && resolvedPath == referencePath;
}

bool ResolvedAssetLocalIdentifierMatches(
    const PrefabResolvedAsset& resolved,
    const Serialize::ObjectIdentifier& reference)
{
    if (resolved.subAssetKey.empty() || reference.localIdentifierInFile == 0)
        return false;

    return Serialize::MakeLocalIdentifierInFile(
        reference.guid,
        resolved.subAssetKey) == reference.localIdentifierInFile;
}

std::string MakeResolvedAssetPathKey(const std::string& assetId, const std::string& path)
{
    return assetId + "\npath:" + path;
}

std::string MakeResolvedAssetLocalIdentifierKey(const std::string& assetId, const uint64_t localIdentifierInFile)
{
    return assetId + "\nlocal:" + std::to_string(localIdentifierInFile);
}

void AddResolvedAssetPathKey(
    PrefabResolvedAssetValidationIndex& index,
    const std::string& assetId,
    const std::string& path)
{
    if (path.empty())
        return;

    index.referenceKeys.insert(MakeResolvedAssetPathKey(assetId, path));
    const auto normalizedPath = std::filesystem::path(path).lexically_normal().generic_string();
    if (!normalizedPath.empty())
        index.referenceKeys.insert(MakeResolvedAssetPathKey(assetId, normalizedPath));
}

PrefabResolvedAssetValidationIndex BuildResolvedAssetValidationIndex(const PrefabArtifact& artifact)
{
    PrefabResolvedAssetValidationIndex index;
    index.assetIdsWithResolvedAssets.reserve(artifact.resolvedAssets.size());
    index.referenceKeys.reserve(artifact.resolvedAssets.size() * 4u);

    for (const auto& resolved : artifact.resolvedAssets)
    {
        const auto assetId = resolved.assetId.ToString();
        if (assetId.empty())
            continue;

        index.assetIdsWithResolvedAssets.insert(assetId);
        AddResolvedAssetPathKey(index, assetId, resolved.subAssetKey);
        AddResolvedAssetPathKey(index, assetId, resolved.artifactPath);

        if (!resolved.subAssetKey.empty())
        {
            index.referenceKeys.insert(MakeResolvedAssetLocalIdentifierKey(
                assetId,
                Serialize::MakeLocalIdentifierInFile(resolved.assetId.GetGuid(), resolved.subAssetKey)));
        }
    }

    return index;
}

bool HasResolvedAsset(
    const PrefabResolvedAssetValidationIndex& index,
    const Serialize::ObjectIdentifier& reference,
    size_t& lookupCount)
{
    ++lookupCount;
    const auto assetId = reference.guid.ToString();
    if (assetId.empty())
        return false;

    if (reference.filePath.empty())
        return index.assetIdsWithResolvedAssets.find(assetId) != index.assetIdsWithResolvedAssets.end();

    if (index.referenceKeys.find(MakeResolvedAssetPathKey(assetId, reference.filePath)) != index.referenceKeys.end())
        return true;

    const auto referencePath = std::filesystem::path(reference.filePath).lexically_normal().generic_string();
    if (!referencePath.empty() &&
        index.referenceKeys.find(MakeResolvedAssetPathKey(assetId, referencePath)) != index.referenceKeys.end())
    {
        return true;
    }

    if (reference.localIdentifierInFile != 0u &&
        index.referenceKeys.find(MakeResolvedAssetLocalIdentifierKey(assetId, reference.localIdentifierInFile)) !=
            index.referenceKeys.end())
    {
        return true;
    }

    return false;
}

void ValidateResolvedAssetValue(
    const PrefabResolvedAssetValidationIndex& resolvedAssetIndex,
    const Serialize::PropertyValue& value,
    Serialize::SerializationDiagnosticList& diagnostics,
    size_t& lookupCount)
{
    switch (value.GetKind())
    {
    case Serialize::PropertyValue::Kind::ObjectReference:
        if (value.GetObjectReference().guid.IsValid() &&
            !HasResolvedAsset(resolvedAssetIndex, value.GetObjectReference(), lookupCount))
        {
            AddDiagnostic(
                diagnostics,
                Serialize::SerializationDiagnosticCode::MissingAsset,
                Serialize::SerializationDiagnosticSeverity::Error,
                "Prefab artifact contains an unresolved asset reference.");
        }
        break;
    case Serialize::PropertyValue::Kind::Array:
        for (const auto& item : value.GetArray())
            ValidateResolvedAssetValue(resolvedAssetIndex, item, diagnostics, lookupCount);
        break;
    case Serialize::PropertyValue::Kind::Object:
        for (const auto& property : value.GetObject())
            ValidateResolvedAssetValue(resolvedAssetIndex, property.second, diagnostics, lookupCount);
        break;
    default:
        break;
    }
}

PrefabResolvedAssetValidationStats ValidateResolvedAssetReferences(
    const PrefabArtifact& artifact,
    Serialize::SerializationDiagnosticList& diagnostics)
{
    const auto resolvedAssetIndex = BuildResolvedAssetValidationIndex(artifact);
    PrefabResolvedAssetValidationStats stats;
    stats.indexKeyCount = resolvedAssetIndex.assetIdsWithResolvedAssets.size() +
        resolvedAssetIndex.referenceKeys.size();
    for (const auto& object : artifact.graph.objects)
    {
        for (const auto& property : object.properties)
            ValidateResolvedAssetValue(resolvedAssetIndex, property.value, diagnostics, stats.lookupCount);
    }
    return stats;
}

void CollectAssetReferencesFromValue(
    const Serialize::PropertyValue& value,
    std::vector<PrefabAssetReference>& references,
    const std::string& expectedType)
{
    switch (value.GetKind())
    {
    case Serialize::PropertyValue::Kind::ObjectReference:
        if (value.GetObjectReference().guid.IsValid())
            references.push_back({value.GetObjectReference(), expectedType});
        break;
    case Serialize::PropertyValue::Kind::Array:
        for (const auto& item : value.GetArray())
            CollectAssetReferencesFromValue(item, references, expectedType);
        break;
    case Serialize::PropertyValue::Kind::Object:
        for (const auto& property : value.GetObject())
            CollectAssetReferencesFromValue(property.second, references, expectedType);
        break;
    default:
        break;
    }
}

bool TypeNameHasSuffix(const std::string& typeName, const std::string_view suffix)
{
    return typeName.size() >= suffix.size() &&
        typeName.compare(typeName.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string InferResolvedAssetTypeFromPropertyContext(
    const Serialize::ObjectRecord& object,
    const std::string& propertyName)
{
    if (TypeNameHasSuffix(object.typeName, "MeshFilter") && propertyName == "mesh")
        return "Mesh";
    if (TypeNameHasSuffix(object.typeName, "MeshRenderer") && propertyName == "materials")
        return "Material";
    return {};
}

std::string InferResolvedAssetType(const PrefabAssetReference& assetReference)
{
    if (!assetReference.expectedType.empty())
        return assetReference.expectedType;

    const auto& path = assetReference.reference.filePath;
    if (path.rfind("mesh:", 0) == 0)
        return "Mesh";
    if (path.rfind("material:", 0) == 0)
        return "Material";
    if (path.rfind("texture:", 0) == 0)
        return "Texture";
    if (path.rfind("shader:", 0) == 0)
        return "Shader";
    if (path.rfind("prefab:", 0) == 0)
        return "Prefab";
    return "Asset";
}

bool ResolvedAssetMatchesReference(
    const PrefabResolvedAsset& resolved,
    const Serialize::ObjectIdentifier& reference)
{
    if (resolved.assetId != NLS::Core::Assets::AssetId(reference.guid))
        return false;

    if (reference.filePath.empty())
        return resolved.subAssetKey.empty() && resolved.artifactPath.empty();

    return ResolvedAssetReferencePathMatches(resolved, reference.filePath) ||
        ResolvedAssetLocalIdentifierMatches(resolved, reference);
}

std::optional<PrefabResolvedAsset> FindExistingResolvedAssetForReference(
    const std::vector<PrefabResolvedAsset>& resolvedAssets,
    const Serialize::ObjectIdentifier& reference)
{
    if (reference.filePath.empty())
    {
        const auto assetId = NLS::Core::Assets::AssetId(reference.guid);
        const PrefabResolvedAsset* candidate = nullptr;
        for (const auto& resolved : resolvedAssets)
        {
            if (resolved.assetId != assetId)
                continue;

            if (!resolved.subAssetKey.empty() || !resolved.artifactPath.empty())
            {
                if (candidate)
                    return std::nullopt;
                candidate = &resolved;
                continue;
            }

            return resolved;
        }
        if (!candidate)
            return std::nullopt;
        return *candidate;
    }

    const auto found = std::find_if(
        resolvedAssets.begin(),
        resolvedAssets.end(),
        [&](const PrefabResolvedAsset& resolved)
        {
            return ResolvedAssetMatchesReference(resolved, reference);
        });
    if (found == resolvedAssets.end())
        return std::nullopt;
    return *found;
}

PrefabResolvedAsset BuildFallbackResolvedAsset(const PrefabAssetReference& assetReference)
{
    PrefabResolvedAsset resolved;
    const auto& reference = assetReference.reference;
    resolved.assetId = NLS::Core::Assets::AssetId(reference.guid);
    resolved.expectedType = InferResolvedAssetType(assetReference);
    resolved.subAssetKey = reference.filePath;
    if (NLS::Core::Assets::IsContentStorageArtifactPath(reference.filePath))
        resolved.artifactPath = reference.filePath;
    return resolved;
}

bool ContainsResolvedAssetReference(
    const std::vector<PrefabResolvedAsset>& resolvedAssets,
    const PrefabResolvedAsset& candidate)
{
    return std::any_of(
        resolvedAssets.begin(),
        resolvedAssets.end(),
        [&](const PrefabResolvedAsset& existing)
        {
            return existing.assetId == candidate.assetId &&
                existing.subAssetKey == candidate.subAssetKey &&
                existing.artifactPath == candidate.artifactPath;
        });
}

struct PrefabResolvedAssetIndex
{
    std::unordered_map<NLS::Core::Assets::AssetId, std::vector<const PrefabResolvedAsset*>> byAssetId;
};

struct PrefabRuntimeResolvedAssetCandidates
{
    const PrefabResolvedAsset* directAsset = nullptr;
    const PrefabResolvedAsset* singleSubAsset = nullptr;
    bool ambiguousSubAssets = false;
};

struct PrefabRuntimeResolvedAssetIndex
{
    std::unordered_map<std::string, const PrefabResolvedAsset*> byReferenceKey;
    std::unordered_map<NLS::Core::Assets::AssetId, PrefabRuntimeResolvedAssetCandidates> emptyReferenceCandidates;
};

struct RuntimeResolvedGraphStats
{
    size_t propertyValueVisitCount = 0u;
    size_t objectReferenceLookupCount = 0u;
    size_t resolvedReferenceCount = 0u;
    size_t indexKeyCount = 0u;
};

PrefabResolvedAssetIndex BuildPrefabResolvedAssetIndex(
    const std::vector<PrefabResolvedAsset>& resolvedAssets)
{
    PrefabResolvedAssetIndex index;
    index.byAssetId.reserve(resolvedAssets.size());
    for (const auto& resolved : resolvedAssets)
        index.byAssetId[resolved.assetId].push_back(&resolved);
    return index;
}

std::optional<PrefabResolvedAsset> FindExistingResolvedAssetForReference(
    const PrefabResolvedAssetIndex& index,
    const Serialize::ObjectIdentifier& reference)
{
    const auto assetId = NLS::Core::Assets::AssetId(reference.guid);
    const auto foundCandidates = index.byAssetId.find(assetId);
    if (foundCandidates == index.byAssetId.end())
        return std::nullopt;

    const auto& candidates = foundCandidates->second;
    if (reference.filePath.empty())
    {
        const PrefabResolvedAsset* candidate = nullptr;
        for (const auto* resolved : candidates)
        {
            if (resolved == nullptr)
                continue;

            if (!resolved->subAssetKey.empty() || !resolved->artifactPath.empty())
            {
                if (candidate)
                    return std::nullopt;
                candidate = resolved;
                continue;
            }

            return *resolved;
        }
        if (!candidate)
            return std::nullopt;
        return *candidate;
    }

    for (const auto* resolved : candidates)
    {
        if (resolved != nullptr && ResolvedAssetMatchesReference(*resolved, reference))
            return *resolved;
    }
    return std::nullopt;
}

struct PrefabResolvedAssetKey
{
    NLS::Core::Assets::AssetId assetId;
    std::string subAssetKey;
    std::string artifactPath;

    bool operator==(const PrefabResolvedAssetKey& other) const = default;
};

struct PrefabResolvedAssetKeyHash
{
    size_t operator()(const PrefabResolvedAssetKey& key) const noexcept
    {
        size_t hash = std::hash<NLS::Core::Assets::AssetId> {}(key.assetId);
        const auto combine = [&hash](const size_t value)
        {
            hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
        };
        combine(std::hash<std::string> {}(key.subAssetKey));
        combine(std::hash<std::string> {}(key.artifactPath));
        return hash;
    }
};

PrefabResolvedAssetKey MakePrefabResolvedAssetKey(const PrefabResolvedAsset& resolved)
{
    return {resolved.assetId, resolved.subAssetKey, resolved.artifactPath};
}

void AddRuntimeResolvedAssetKey(
    PrefabRuntimeResolvedAssetIndex& index,
    const std::string& key,
    const PrefabResolvedAsset& resolved)
{
    if (key.empty())
        return;

    if (index.byReferenceKey.try_emplace(key, &resolved).second)
        return;
}

void AddRuntimeResolvedAssetPathKey(
    PrefabRuntimeResolvedAssetIndex& index,
    const std::string& assetId,
    const std::string& path,
    const PrefabResolvedAsset& resolved)
{
    if (path.empty())
        return;

    AddRuntimeResolvedAssetKey(index, MakeResolvedAssetPathKey(assetId, path), resolved);
}

PrefabRuntimeResolvedAssetIndex BuildPrefabRuntimeResolvedAssetIndex(
    const std::vector<PrefabResolvedAsset>& resolvedAssets)
{
    PrefabRuntimeResolvedAssetIndex index;
    index.byReferenceKey.reserve(resolvedAssets.size() * 4u);
    index.emptyReferenceCandidates.reserve(resolvedAssets.size());

    for (const auto& resolved : resolvedAssets)
    {
        const auto assetId = resolved.assetId.ToString();
        if (assetId.empty())
            continue;

        if (resolved.subAssetKey.empty() && resolved.artifactPath.empty())
        {
            auto& candidates = index.emptyReferenceCandidates[resolved.assetId];
            if (candidates.directAsset == nullptr)
                candidates.directAsset = &resolved;
            continue;
        }

        auto& candidates = index.emptyReferenceCandidates[resolved.assetId];
        if (!candidates.ambiguousSubAssets)
        {
            if (candidates.singleSubAsset == nullptr)
            {
                candidates.singleSubAsset = &resolved;
            }
            else
            {
                candidates.singleSubAsset = nullptr;
                candidates.ambiguousSubAssets = true;
            }
        }

        AddRuntimeResolvedAssetPathKey(index, assetId, resolved.subAssetKey, resolved);
        AddRuntimeResolvedAssetPathKey(index, assetId, resolved.artifactPath, resolved);

        if (!resolved.artifactPath.empty())
        {
            const auto normalizedPath = std::filesystem::path(resolved.artifactPath).lexically_normal().generic_string();
            AddRuntimeResolvedAssetPathKey(index, assetId, normalizedPath, resolved);
        }

        if (!resolved.subAssetKey.empty())
        {
            AddRuntimeResolvedAssetKey(
                index,
                MakeResolvedAssetLocalIdentifierKey(
                    assetId,
                    Serialize::MakeLocalIdentifierInFile(resolved.assetId.GetGuid(), resolved.subAssetKey)),
                resolved);
        }
    }

    return index;
}

bool VisitBaseChain(
    const PrefabArtifact& artifact,
    const std::unordered_map<NLS::Core::Assets::AssetId, const PrefabArtifact*>& artifactsById,
    std::unordered_set<NLS::Core::Assets::AssetId>& visiting,
    std::unordered_set<NLS::Core::Assets::AssetId>& visited,
    Serialize::SerializationDiagnosticList& diagnostics)
{
    if (!artifact.assetId.IsValid())
        return true;

    if (visited.find(artifact.assetId) != visited.end())
        return true;

    if (!visiting.insert(artifact.assetId).second)
    {
        AddDiagnostic(
            diagnostics,
            Serialize::SerializationDiagnosticCode::InvalidPrefabOverride,
            Serialize::SerializationDiagnosticSeverity::Error,
            "Prefab base chain contains a cycle.");
        return false;
    }

    for (const auto& baseId : artifact.baseChain)
    {
        const auto foundBase = artifactsById.find(baseId);
        if (foundBase == artifactsById.end())
        {
            AddDiagnostic(
                diagnostics,
                Serialize::SerializationDiagnosticCode::MissingAsset,
                Serialize::SerializationDiagnosticSeverity::Error,
                "Prefab base chain references a missing base prefab.");
            continue;
        }

        VisitBaseChain(*foundBase->second, artifactsById, visiting, visited, diagnostics);
    }

    visiting.erase(artifact.assetId);
    visited.insert(artifact.assetId);
    return true;
}

int PatchTypeOrder(Serialize::PatchOperationType type)
{
    switch (type)
    {
    case Serialize::PatchOperationType::ReplaceProperty:
        return 0;
    case Serialize::PatchOperationType::InsertOwned:
        return 1;
    case Serialize::PatchOperationType::RemoveOwned:
        return 2;
    case Serialize::PatchOperationType::MoveOwned:
        return 3;
    case Serialize::PatchOperationType::AddPrefabInstance:
        return 4;
    case Serialize::PatchOperationType::RemoveObject:
        return 5;
    default:
        return 6;
    }
}

std::string PatchObjectKey(const Serialize::ObjectId& id)
{
    return id.IsValid() ? id.GetGuid().ToString() : std::string {};
}

std::string PatchDedupeKey(const Serialize::PatchOperation& patch)
{
    std::string key;
    key += std::to_string(PatchTypeOrder(patch.type));
    key += "|";
    key += PatchObjectKey(patch.target);
    key += "|";
    key += patch.property;

    if (patch.type != Serialize::PatchOperationType::ReplaceProperty)
    {
        key += "|";
        key += PatchObjectKey(patch.object);
    }

    return key;
}

bool IsPatchLess(const Serialize::PatchOperation& lhs, const Serialize::PatchOperation& rhs)
{
    const auto lhsTypeOrder = PatchTypeOrder(lhs.type);
    const auto rhsTypeOrder = PatchTypeOrder(rhs.type);
    if (lhsTypeOrder != rhsTypeOrder)
        return lhsTypeOrder < rhsTypeOrder;

    if (lhs.target != rhs.target)
        return lhs.target < rhs.target;

    if (lhs.property != rhs.property)
        return lhs.property < rhs.property;

    if (lhs.object != rhs.object)
        return lhs.object < rhs.object;

    if (lhs.hasIndex != rhs.hasIndex)
        return lhs.hasIndex && !rhs.hasIndex;

    return lhs.index < rhs.index;
}

bool IsNestedPrefabReferenceProperty(std::string_view propertyName)
{
    return propertyName == "nestedPrefab";
}

bool IsResolvedPrefabReference(
    const PrefabArtifact& artifact,
    const Serialize::ObjectIdentifier& reference)
{
    return std::any_of(
        artifact.resolvedAssets.begin(),
        artifact.resolvedAssets.end(),
        [&reference](const PrefabResolvedAsset& resolved)
        {
            return resolved.assetId == NLS::Core::Assets::AssetId(reference.guid) &&
                resolved.expectedType == "Prefab" &&
                (reference.filePath.empty() ||
                    ResolvedAssetReferencePathMatches(resolved, reference.filePath));
        });
}

void CollectNestedPrefabDependenciesFromValue(
    const PrefabArtifact& artifact,
    const Serialize::PropertyValue& value,
    std::vector<NLS::Core::Assets::AssetId>& dependencies,
    std::string_view propertyName = {})
{
    switch (value.GetKind())
    {
    case Serialize::PropertyValue::Kind::ObjectReference:
    {
        const auto& reference = value.GetObjectReference();
        if (reference.guid.IsValid() &&
            (IsNestedPrefabReferenceProperty(propertyName) ||
                IsResolvedPrefabReference(artifact, reference)))
        {
            dependencies.emplace_back(reference.guid);
        }
        break;
    }
    case Serialize::PropertyValue::Kind::Array:
        for (const auto& item : value.GetArray())
            CollectNestedPrefabDependenciesFromValue(artifact, item, dependencies, propertyName);
        break;
    case Serialize::PropertyValue::Kind::Object:
        for (const auto& property : value.GetObject())
            CollectNestedPrefabDependenciesFromValue(artifact, property.second, dependencies, property.first);
        break;
    default:
        break;
    }
}

const PrefabResolvedAsset* FindRuntimeResolvedAsset(
    const PrefabRuntimeResolvedAssetIndex& index,
    const Serialize::ObjectIdentifier& reference)
{
    const auto assetId = NLS::Core::Assets::AssetId(reference.guid);
    const auto assetIdString = reference.guid.ToString();
    if (assetIdString.empty())
        return nullptr;

    if (reference.filePath.empty())
    {
        const auto found = index.emptyReferenceCandidates.find(assetId);
        if (found == index.emptyReferenceCandidates.end())
            return nullptr;

        const auto& candidates = found->second;
        if (candidates.directAsset != nullptr)
            return candidates.directAsset;
        if (!candidates.ambiguousSubAssets)
            return candidates.singleSubAsset;
        return nullptr;
    }

    const auto foundPath = index.byReferenceKey.find(MakeResolvedAssetPathKey(assetIdString, reference.filePath));
    if (foundPath != index.byReferenceKey.end())
        return foundPath->second;

    const auto normalizedPath = std::filesystem::path(reference.filePath).lexically_normal().generic_string();
    if (!normalizedPath.empty())
    {
        const auto foundNormalizedPath = index.byReferenceKey.find(MakeResolvedAssetPathKey(assetIdString, normalizedPath));
        if (foundNormalizedPath != index.byReferenceKey.end())
            return foundNormalizedPath->second;
    }

    if (reference.localIdentifierInFile != 0u)
    {
        const auto foundLocalIdentifier = index.byReferenceKey.find(
            MakeResolvedAssetLocalIdentifierKey(assetIdString, reference.localIdentifierInFile));
        if (foundLocalIdentifier != index.byReferenceKey.end())
            return foundLocalIdentifier->second;
    }

    return nullptr;
}

void ResolveObjectIdentifierInPlace(
    const PrefabRuntimeResolvedAssetIndex& index,
    Serialize::PropertyValue& value,
    RuntimeResolvedGraphStats* stats)
{
    if (stats != nullptr)
        ++stats->propertyValueVisitCount;

    switch (value.GetKind())
    {
    case Serialize::PropertyValue::Kind::ObjectReference:
    {
        auto& reference = value.GetMutableObjectReference();
        if (!reference.guid.IsValid())
            return;
        if (stats != nullptr)
            ++stats->objectReferenceLookupCount;
        if (const auto* resolved = FindRuntimeResolvedAsset(index, reference);
            resolved && !resolved->artifactPath.empty())
        {
            reference.filePath = resolved->artifactPath;
            if (stats != nullptr)
                ++stats->resolvedReferenceCount;
        }
        break;
    }
    case Serialize::PropertyValue::Kind::Array:
    {
        for (auto& item : value.GetMutableArray())
            ResolveObjectIdentifierInPlace(index, item, stats);
        break;
    }
    case Serialize::PropertyValue::Kind::Object:
    {
        for (auto& property : value.GetMutableObject())
            ResolveObjectIdentifierInPlace(index, property.second, stats);
        break;
    }
    default:
        break;
    }
}

uint64_t HashCombine(const uint64_t seed, const uint64_t value)
{
    return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u));
}

uint64_t HashString(const std::string_view value)
{
    uint64_t hash = 1469598103934665603ull;
    for (const auto character : value)
    {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(character));
        hash *= 1099511628211ull;
    }
    return hash;
}

uint64_t HashGuid(const NLS::Guid& guid)
{
    return static_cast<uint64_t>(std::hash<NLS::Guid> {}(guid));
}

uint64_t HashObjectId(const Serialize::ObjectId& id)
{
    return HashGuid(id.GetGuid());
}

uint64_t HashObjectIdentifier(const Serialize::ObjectIdentifier& identifier)
{
    uint64_t hash = HashGuid(identifier.guid);
    hash = HashCombine(hash, static_cast<uint64_t>(identifier.localIdentifierInFile));
    hash = HashCombine(hash, static_cast<uint64_t>(identifier.fileType));
    hash = HashCombine(hash, HashString(identifier.filePath));
    return hash;
}

uint64_t HashPropertyValue(const Serialize::PropertyValue& value)
{
    uint64_t hash = static_cast<uint64_t>(value.GetKind());
    switch (value.GetKind())
    {
    case Serialize::PropertyValue::Kind::Null:
        break;
    case Serialize::PropertyValue::Kind::Bool:
        hash = HashCombine(hash, value.GetBool() ? 1u : 0u);
        break;
    case Serialize::PropertyValue::Kind::Integer:
        hash = HashCombine(hash, static_cast<uint64_t>(value.GetInteger()));
        break;
    case Serialize::PropertyValue::Kind::Number:
        hash = HashCombine(hash, static_cast<uint64_t>(std::hash<double> {}(value.GetNumber())));
        break;
    case Serialize::PropertyValue::Kind::String:
        hash = HashCombine(hash, HashString(value.GetString()));
        break;
    case Serialize::PropertyValue::Kind::Guid:
        hash = HashCombine(hash, HashGuid(value.GetGuid()));
        break;
    case Serialize::PropertyValue::Kind::OwnedReference:
        hash = HashCombine(hash, HashObjectId(value.GetObjectId()));
        break;
    case Serialize::PropertyValue::Kind::ObjectReference:
        hash = HashCombine(hash, HashObjectIdentifier(value.GetObjectReference()));
        break;
    case Serialize::PropertyValue::Kind::Array:
        for (const auto& item : value.GetArray())
            hash = HashCombine(hash, HashPropertyValue(item));
        break;
    case Serialize::PropertyValue::Kind::Object:
        for (const auto& property : value.GetObject())
        {
            hash = HashCombine(hash, HashString(property.first));
            hash = HashCombine(hash, HashPropertyValue(property.second));
        }
        break;
    }
    return hash;
}

uint64_t HashPatchOperation(const Serialize::PatchOperation& patch)
{
    uint64_t hash = static_cast<uint64_t>(patch.type);
    hash = HashCombine(hash, HashObjectId(patch.target));
    hash = HashCombine(hash, HashString(patch.property));
    hash = HashCombine(hash, HashPropertyValue(patch.value));
    hash = HashCombine(hash, HashObjectId(patch.object));
    hash = HashCombine(hash, static_cast<uint64_t>(patch.index));
    hash = HashCombine(hash, patch.hasIndex ? 1u : 0u);
    return hash;
}

uint64_t HashObjectRecord(const Serialize::ObjectRecord& object)
{
    uint64_t hash = HashObjectId(object.id);
    hash = HashCombine(hash, HashString(object.typeName));
    hash = HashCombine(hash, HashString(object.debugName));
    hash = HashCombine(hash, HashString(object.debugPath));
    hash = HashCombine(hash, static_cast<uint64_t>(object.state));
    hash = HashCombine(hash, static_cast<uint64_t>(object.localIdentifierInFile));
    hash = HashCombine(hash, static_cast<uint64_t>(object.properties.size()));
    for (const auto& property : object.properties)
    {
        hash = HashCombine(hash, HashString(property.name));
        hash = HashCombine(hash, HashPropertyValue(property.value));
    }
    return hash;
}

uint64_t HashPrefabInstanceRecord(const Serialize::PrefabInstanceRecord& instance)
{
    uint64_t hash = HashObjectId(instance.instanceRoot);
    hash = HashCombine(hash, HashObjectIdentifier(instance.sourcePrefab));
    hash = HashCombine(hash, instance.generatedReadOnly ? 1u : 0u);
    for (const auto& modification : instance.modifications)
        hash = HashCombine(hash, HashPatchOperation(modification));
    for (const auto& object : instance.addedObjects)
        hash = HashCombine(hash, HashObjectRecord(object));
    for (const auto& correspondence : instance.correspondence)
    {
        hash = HashCombine(hash, HashObjectId(correspondence.sourceObject));
        hash = HashCombine(hash, HashObjectId(correspondence.instanceObject));
    }
    return hash;
}

uint64_t HashObjectGraphDocument(const Serialize::ObjectGraphDocument& graph)
{
    uint64_t hash = HashString(graph.format);
    hash = HashCombine(hash, static_cast<uint64_t>(graph.version));
    hash = HashCombine(hash, HashGuid(graph.documentId));
    hash = HashCombine(hash, HashObjectId(graph.root));
    hash = HashCombine(hash, graph.basePrefab.has_value() ? 1u : 0u);
    if (graph.basePrefab.has_value())
        hash = HashCombine(hash, HashObjectIdentifier(*graph.basePrefab));
    hash = HashCombine(hash, static_cast<uint64_t>(graph.objects.size()));
    for (const auto& object : graph.objects)
        hash = HashCombine(hash, HashObjectRecord(object));
    hash = HashCombine(hash, static_cast<uint64_t>(graph.overrides.size()));
    for (const auto& overrideOperation : graph.overrides)
        hash = HashCombine(hash, HashPatchOperation(overrideOperation));
    hash = HashCombine(hash, static_cast<uint64_t>(graph.prefabInstances.size()));
    for (const auto& instance : graph.prefabInstances)
        hash = HashCombine(hash, HashPrefabInstanceRecord(instance));
    return hash;
}

uint64_t HashPrefabResolvedAsset(const PrefabResolvedAsset& resolved)
{
    uint64_t hash = HashGuid(resolved.assetId.GetGuid());
    hash = HashCombine(hash, HashString(resolved.expectedType));
    hash = HashCombine(hash, HashString(resolved.subAssetKey));
    hash = HashCombine(hash, HashString(resolved.artifactPath));
    return hash;
}

uint64_t HashPrefabValidationResolvedAsset(const PrefabResolvedAsset& resolved)
{
    uint64_t hash = HashGuid(resolved.assetId.GetGuid());
    hash = HashCombine(hash, HashString(resolved.expectedType));
    hash = HashCombine(hash, HashString(resolved.subAssetKey));
    return hash;
}

uint64_t BuildRuntimeResolvedGraphFingerprint(const PrefabArtifact& artifact)
{
    uint64_t hash = HashObjectGraphDocument(artifact.graph);
    hash = HashCombine(hash, static_cast<uint64_t>(artifact.resolvedAssets.size()));
    for (const auto& resolved : artifact.resolvedAssets)
        hash = HashCombine(hash, HashPrefabResolvedAsset(resolved));
    return hash == 0u ? 1u : hash;
}

uint64_t BuildValidationFingerprint(const PrefabArtifact& artifact)
{
    uint64_t hash = HashObjectGraphDocument(artifact.graph);
    auto resolvedAssets = artifact.resolvedAssets;
    std::sort(
        resolvedAssets.begin(),
        resolvedAssets.end(),
        [](const PrefabResolvedAsset& lhs, const PrefabResolvedAsset& rhs)
        {
            if (lhs.assetId != rhs.assetId)
                return lhs.assetId < rhs.assetId;
            if (lhs.expectedType != rhs.expectedType)
                return lhs.expectedType < rhs.expectedType;
            return lhs.subAssetKey < rhs.subAssetKey;
        });
    hash = HashCombine(hash, static_cast<uint64_t>(resolvedAssets.size()));
    for (const auto& resolved : resolvedAssets)
        hash = HashCombine(hash, HashPrefabValidationResolvedAsset(resolved));
    return hash == 0u ? 1u : hash;
}

Serialize::ObjectGraphDocument BuildRuntimeResolvedGraph(
    const PrefabArtifact& artifact,
    RuntimeResolvedGraphStats* stats = nullptr)
{
    auto graph = artifact.graph;
    const auto resolvedAssetIndex = BuildPrefabRuntimeResolvedAssetIndex(artifact.resolvedAssets);
    if (stats != nullptr)
        stats->indexKeyCount = resolvedAssetIndex.byReferenceKey.size();
    for (auto& object : graph.objects)
    {
        for (auto& property : object.properties)
            ResolveObjectIdentifierInPlace(resolvedAssetIndex, property.value, stats);
    }
    return graph;
}

void AddRuntimeResolvedGraphStatsCounters(
    NLS::Base::Profiling::PerformanceStageScope& scope,
    const RuntimeResolvedGraphStats& stats)
{
    scope.AddCounter("runtimeResolvedGraphIndexKeyCount", stats.indexKeyCount);
    scope.AddCounter("runtimeResolvedGraphPropertyValueVisitCount", stats.propertyValueVisitCount);
    scope.AddCounter("runtimeResolvedGraphObjectReferenceLookupCount", stats.objectReferenceLookupCount);
    scope.AddCounter("runtimeResolvedGraphResolvedReferenceCount", stats.resolvedReferenceCount);
}

std::mutex& RuntimeResolvedGraphCacheMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::mutex& PrefabInstantiatePlanCacheMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::mutex& PrefabValidationCacheMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::shared_ptr<const Serialize::ObjectGraphDocument> GetOrBuildRuntimeResolvedGraph(
    const PrefabArtifact& artifact,
    const uint64_t fingerprint,
    bool& cacheHit,
    RuntimeResolvedGraphStats* stats = nullptr)
{
    {
        std::lock_guard<std::mutex> lock(RuntimeResolvedGraphCacheMutex());
        if (artifact.runtimeResolvedGraph &&
            artifact.runtimeResolvedGraphFingerprint == fingerprint)
        {
            cacheHit = true;
            return artifact.runtimeResolvedGraph;
        }
    }

    auto graph = std::make_shared<Serialize::ObjectGraphDocument>(
        BuildRuntimeResolvedGraph(artifact, stats));

    {
        std::lock_guard<std::mutex> lock(RuntimeResolvedGraphCacheMutex());
        if (artifact.runtimeResolvedGraph &&
            artifact.runtimeResolvedGraphFingerprint == fingerprint)
        {
            cacheHit = true;
            return artifact.runtimeResolvedGraph;
        }

        artifact.runtimeResolvedGraph = graph;
        artifact.runtimeResolvedGraphFingerprint = fingerprint;
    }

    cacheHit = false;
    return graph;
}

std::shared_ptr<const Serialize::ObjectGraphInstantiator::PrefabInstantiatePlan> GetOrBuildPrefabInstantiatePlan(
    const PrefabArtifact& artifact,
    const Serialize::ObjectGraphDocument& graph,
    const uint64_t fingerprint,
    bool& cacheHit)
{
    {
        std::lock_guard<std::mutex> lock(PrefabInstantiatePlanCacheMutex());
        if (artifact.instantiatePlan &&
            artifact.instantiatePlanFingerprint == fingerprint)
        {
            cacheHit = true;
            return artifact.instantiatePlan;
        }
    }

    auto plan = std::make_shared<Serialize::ObjectGraphInstantiator::PrefabInstantiatePlan>(
        Serialize::ObjectGraphInstantiator::BuildPrefabInstantiatePlan(graph));

    {
        std::lock_guard<std::mutex> lock(PrefabInstantiatePlanCacheMutex());
        if (artifact.instantiatePlan &&
            artifact.instantiatePlanFingerprint == fingerprint)
        {
            cacheHit = true;
            return artifact.instantiatePlan;
        }

        artifact.instantiatePlan = plan;
        artifact.instantiatePlanFingerprint = fingerprint;
    }

    cacheHit = false;
    return plan;
}

void PrewarmPrefabMeshArtifacts(
    const PrefabArtifact& artifact,
    NLS::Base::Profiling::PerformanceStageScope* uploadScope)
{
    if (!Core::ServiceLocator::Contains<Core::ResourceManagement::MeshManager>())
        return;

    auto& meshManager = NLS_SERVICE(Core::ResourceManagement::MeshManager);
    for (const auto& resolved : artifact.resolvedAssets)
    {
        if (resolved.expectedType == "Mesh" && !resolved.artifactPath.empty())
        {
            meshManager.PrewarmArtifact(resolved.artifactPath);
            if (uploadScope != nullptr)
                uploadScope->AddCounter("synchronousResourceLoadCount");
        }
    }
}

void PrewarmPrefabMaterialArtifacts(
    const PrefabArtifact& artifact,
    NLS::Base::Profiling::PerformanceStageScope* uploadScope)
{
    if (!Core::ServiceLocator::Contains<Core::ResourceManagement::MaterialManager>())
        return;

    auto& materialManager = NLS_SERVICE(Core::ResourceManagement::MaterialManager);
    for (const auto& resolved : artifact.resolvedAssets)
    {
        if (resolved.expectedType == "Material" && !resolved.artifactPath.empty())
        {
            materialManager.PrewarmArtifactWithDependencies(resolved.artifactPath);
            if (uploadScope != nullptr)
                uploadScope->AddCounter("synchronousResourceLoadCount");
        }
    }
}

bool VisitNestedPrefabDependencies(
    const PrefabArtifact& artifact,
    const std::unordered_map<NLS::Core::Assets::AssetId, const PrefabArtifact*>& artifactsById,
    std::unordered_set<NLS::Core::Assets::AssetId>& visiting,
    std::unordered_set<NLS::Core::Assets::AssetId>& visited,
    Serialize::SerializationDiagnosticList& diagnostics)
{
    if (!artifact.assetId.IsValid())
        return true;

    if (visited.find(artifact.assetId) != visited.end())
        return true;

    if (!visiting.insert(artifact.assetId).second)
    {
        AddDiagnostic(
            diagnostics,
            Serialize::SerializationDiagnosticCode::InvalidPrefabOverride,
            Serialize::SerializationDiagnosticSeverity::Error,
            "Nested prefab dependency graph contains a cycle.");
        return false;
    }

    for (const auto& dependency : ExtractNestedPrefabDependencies(artifact))
    {
        const auto foundDependency = artifactsById.find(dependency);
        if (foundDependency == artifactsById.end())
        {
            AddDiagnostic(
                diagnostics,
                Serialize::SerializationDiagnosticCode::MissingAsset,
                Serialize::SerializationDiagnosticSeverity::Error,
                "Nested prefab dependency references a missing prefab.");
            continue;
        }

        VisitNestedPrefabDependencies(*foundDependency->second, artifactsById, visiting, visited, diagnostics);
    }

    visiting.erase(artifact.assetId);
    visited.insert(artifact.assetId);
    return true;
}
}

const Serialize::ObjectId* PrefabArtifact::FindRuntimeObject(const Serialize::ObjectId& sourceObject) const
{
    const auto found = sourceToRuntimeObject.find(sourceObject);
    if (found == sourceToRuntimeObject.end())
        return nullptr;
    return &found->second;
}

Serialize::SerializationDiagnosticList PrefabArtifact::Validate() const
{
    return Validate(BuildRuntimeResolvedGraphFingerprint(*this));
}

Serialize::SerializationDiagnosticList PrefabArtifact::Validate(const uint64_t fingerprint) const
{
    NLS::Base::Profiling::PerformanceStageScope validateScope(
        NLS::Base::Profiling::PerformanceStageDomain::Prefab,
        "ValidatePrefabArtifact",
        NLS::Base::Profiling::PerformanceStageThread::Main);
    validateScope.AddCounter("objectCount", graph.objects.size());
    validateScope.AddCounter("validationGraphCopyCount", 0u);

    {
        std::lock_guard<std::mutex> lock(PrefabValidationCacheMutex());
        if (validationDiagnostics && validationFingerprint == fingerprint)
        {
            validateScope.AddCounter("validationCacheHitCount", 1u);
            return *validationDiagnostics;
        }
    }

    auto diagnostics = Serialize::ObjectGraphInstantiator::ValidatePrefabGraph(graph);
    const auto resolvedAssetStats = ValidateResolvedAssetReferences(*this, diagnostics);
    validateScope.AddCounter("resolvedAssetIndexKeyCount", resolvedAssetStats.indexKeyCount);
    validateScope.AddCounter("resolvedAssetReferenceLookupCount", resolvedAssetStats.lookupCount);
    auto cachedDiagnostics = std::make_shared<Serialize::SerializationDiagnosticList>(diagnostics);
    {
        std::lock_guard<std::mutex> lock(PrefabValidationCacheMutex());
        if (validationDiagnostics && validationFingerprint == fingerprint)
        {
            validateScope.AddCounter("validationCacheHitCount", 1u);
            return *validationDiagnostics;
        }

        validationDiagnostics = std::move(cachedDiagnostics);
        validationFingerprint = fingerprint;
        validateScope.AddCounter("validationCacheMissCount", 1u);
    }
    return diagnostics;
}

uint64_t BuildPrefabArtifactValidationFingerprint(const PrefabArtifact& artifact)
{
    return BuildValidationFingerprint(artifact);
}

uint64_t BuildPrefabRuntimeResolvedGraphFingerprint(const PrefabArtifact& artifact)
{
    return BuildRuntimeResolvedGraphFingerprint(artifact);
}

std::string PrefabResolvedAssetTypeForArtifactType(const NLS::Core::Assets::ArtifactType type)
{
    using NLS::Core::Assets::ArtifactType;
    switch (type)
    {
    case ArtifactType::Mesh: return "Mesh";
    case ArtifactType::Material: return "Material";
    case ArtifactType::Texture: return "Texture";
    case ArtifactType::Skeleton: return "Skeleton";
    case ArtifactType::Skin: return "Skin";
    case ArtifactType::AnimationClip: return "AnimationClip";
    case ArtifactType::MorphTarget: return "MorphTarget";
    case ArtifactType::Model: return "Model";
    case ArtifactType::Shader: return "Shader";
    case ArtifactType::Scene: return "Scene";
    case ArtifactType::Audio: return "Audio";
    case ArtifactType::Prefab:
    case ArtifactType::Unknown:
    case ArtifactType::Count:
        return {};
    }
    return {};
}

std::vector<PrefabResolvedAsset> BuildPrefabValidationResolvedAssetsFromManifest(
    const NLS::Core::Assets::ArtifactManifest& manifest)
{
    std::vector<PrefabResolvedAsset> resolvedAssets;
    for (const auto& artifact : manifest.subAssets)
    {
        if (artifact.artifactType == NLS::Core::Assets::ArtifactType::Prefab)
            continue;

        auto expectedType = PrefabResolvedAssetTypeForArtifactType(artifact.artifactType);
        if (expectedType.empty())
            continue;

        const auto sourceAssetId = artifact.sourceAssetId.IsValid()
            ? artifact.sourceAssetId
            : manifest.sourceAssetId;
        resolvedAssets.push_back({
            sourceAssetId,
            std::move(expectedType),
            artifact.subAssetKey,
            std::filesystem::path(artifact.artifactPath).lexically_normal().generic_string()
        });
    }
    return resolvedAssets;
}

std::string FindPrefabValidationProofFingerprint(
    const std::vector<NLS::Core::Assets::AssetDependencyRecord>& dependencies,
    const std::string_view prefabSubAssetKey)
{
    if (prefabSubAssetKey.empty())
        return {};

    for (const auto& dependency : dependencies)
    {
        if (dependency.kind == NLS::Core::Assets::AssetDependencyKind::PrefabValidation &&
            dependency.value == prefabSubAssetKey &&
            !dependency.hashOrVersion.empty())
        {
            return dependency.hashOrVersion;
        }
    }
    return {};
}

std::vector<Serialize::ObjectIdentifier> CollectPrefabAssetReferences(
    const Serialize::ObjectGraphDocument& graph)
{
    std::vector<PrefabAssetReference> assetReferences;
    for (const auto& object : graph.objects)
    {
        for (const auto& property : object.properties)
        {
            CollectAssetReferencesFromValue(
                property.value,
                assetReferences,
                InferResolvedAssetTypeFromPropertyContext(object, property.name));
        }
    }

    std::vector<Serialize::ObjectIdentifier> references;
    references.reserve(assetReferences.size());
    for (auto& assetReference : assetReferences)
        references.push_back(std::move(assetReference.reference));
    return references;
}

std::string ExtractPrefabAssetReferenceSubAssetKeyHint(const std::string& referencePath)
{
    (void)referencePath;
    return {};
}

std::vector<PrefabAssetReference> CollectPrefabAssetReferenceRecords(
    const Serialize::ObjectGraphDocument& graph)
{
    std::vector<PrefabAssetReference> references;
    for (const auto& object : graph.objects)
    {
        for (const auto& property : object.properties)
        {
            CollectAssetReferencesFromValue(
                property.value,
                references,
                InferResolvedAssetTypeFromPropertyContext(object, property.name));
        }
    }
    return references;
}

std::vector<PrefabResolvedAsset> BuildPrefabResolvedAssetsFromReferences(
    const Serialize::ObjectGraphDocument& graph)
{
    return BuildPrefabResolvedAssetsFromReferences(graph, {});
}

std::vector<PrefabResolvedAsset> BuildPrefabResolvedAssetsFromReferences(
    const Serialize::ObjectGraphDocument& graph,
    const std::vector<PrefabResolvedAsset>& existingResolvedAssets)
{
    std::vector<PrefabResolvedAsset> resolvedAssets;
    auto existingIndex = BuildPrefabResolvedAssetIndex(existingResolvedAssets);
    std::unordered_set<PrefabResolvedAssetKey, PrefabResolvedAssetKeyHash> resolvedKeys;
    for (const auto& assetReference : CollectPrefabAssetReferenceRecords(graph))
    {
        const auto& reference = assetReference.reference;
        auto resolved = FindExistingResolvedAssetForReference(existingIndex, reference);
        auto resolvedAsset = resolved.has_value()
            ? std::move(*resolved)
            : BuildFallbackResolvedAsset(assetReference);
        if (resolvedKeys.insert(MakePrefabResolvedAssetKey(resolvedAsset)).second)
            resolvedAssets.push_back(std::move(resolvedAsset));
    }
    return resolvedAssets;
}

void RefreshPrefabResolvedAssetsFromReferences(PrefabArtifact& artifact)
{
    artifact.resolvedAssets = BuildPrefabResolvedAssetsFromReferences(
        artifact.graph,
        artifact.resolvedAssets);
}

PrefabImportResult ImportPrefabArtifact(
    const std::string& sourceText,
    NLS::Core::Assets::AssetId assetId)
{
    return ImportPrefabArtifact(sourceText, assetId, {});
}

PrefabImportResult ImportPrefabArtifact(
    const std::string& sourceText,
    NLS::Core::Assets::AssetId assetId,
    std::vector<PrefabResolvedAsset> resolvedAssets)
{
    return ImportPrefabArtifact(sourceText, assetId, std::move(resolvedAssets), {});
}

PrefabImportResult ImportPrefabArtifact(
    const std::string& sourceText,
    NLS::Core::Assets::AssetId assetId,
    std::vector<PrefabResolvedAsset> resolvedAssets,
    PrefabImportOptions options)
{
    NLS::Base::Profiling::PerformanceStageScope parseScope(
        NLS::Base::Profiling::PerformanceStageDomain::Prefab,
        "ParsePreparedPrefab",
        NLS::Base::Profiling::PerformanceStageThread::Main);

    PrefabImportResult result;
    result.artifact.assetId = assetId;
    result.artifact.resolvedAssets = std::move(resolvedAssets);

    const auto document = Serialize::ObjectGraphReader::Read(sourceText);
    if (!document.has_value())
    {
        result.diagnostics.Add({
            Serialize::SerializationDiagnosticCode::UnsupportedFormat,
            Serialize::SerializationDiagnosticSeverity::Error,
            "Prefab source could not be parsed as a Nullus object graph document."
        });
        return result;
    }

    result.artifact.graph = std::move(*document);
    if (!options.trustResolvedAssets)
    {
        NLS::Base::Profiling::PerformanceStageScope resolveScope(
            NLS::Base::Profiling::PerformanceStageDomain::Prefab,
            "ResolveDependencies",
            NLS::Base::Profiling::PerformanceStageThread::Main);
        RefreshPrefabResolvedAssetsFromReferences(result.artifact);
        resolveScope.AddCounter("dependencyCount", result.artifact.resolvedAssets.size());
        resolveScope.AddCounter("manifestResolvedAssetsTrusted", 0u);
    }
    else
    {
        NLS::Base::Profiling::PerformanceStageScope resolveScope(
            NLS::Base::Profiling::PerformanceStageDomain::Prefab,
            "ResolveDependencies",
            NLS::Base::Profiling::PerformanceStageThread::Main);
        resolveScope.AddCounter("dependencyCount", result.artifact.resolvedAssets.size());
        resolveScope.AddCounter("manifestResolvedAssetsTrusted", 1u);
    }
    if (result.artifact.graph.basePrefab.has_value())
    {
        result.artifact.baseChain.push_back(
            NLS::Core::Assets::AssetId(result.artifact.graph.basePrefab->guid));
    }

    const auto runtimeResolvedGraphFingerprint = BuildRuntimeResolvedGraphFingerprint(result.artifact);
    auto graphValidationArtifact = result.artifact;
    if (!options.trustedGraphValidationResolvedAssets.empty())
        graphValidationArtifact.resolvedAssets = std::move(options.trustedGraphValidationResolvedAssets);
    const auto artifactFingerprint = BuildPrefabArtifactValidationFingerprint(graphValidationArtifact);
    const bool hasTrustedGraphValidation =
        options.trustGraphValidation &&
        !options.trustedGraphValidationFingerprint.empty() &&
        options.trustedGraphValidationFingerprint == std::to_string(artifactFingerprint);
    if (hasTrustedGraphValidation)
    {
        {
            NLS::Base::Profiling::PerformanceStageScope validateScope(
                NLS::Base::Profiling::PerformanceStageDomain::Prefab,
                "ValidatePrefabArtifact",
                NLS::Base::Profiling::PerformanceStageThread::Main);
            validateScope.AddCounter("objectCount", result.artifact.graph.objects.size());
            validateScope.AddCounter("graphValidationTrusted", 1u);
            result.artifact.validationFingerprint = runtimeResolvedGraphFingerprint;
            result.artifact.validationDiagnostics =
                std::make_shared<const Serialize::SerializationDiagnosticList>();
        }
        if (options.trustResolvedAssets && !result.artifact.resolvedAssets.empty())
        {
            NLS::Base::Profiling::PerformanceStageScope resolveScope(
                NLS::Base::Profiling::PerformanceStageDomain::Prefab,
                "ResolveExternalReferences",
                NLS::Base::Profiling::PerformanceStageThread::Main);
            RuntimeResolvedGraphStats runtimeResolvedGraphStats;
            auto runtimeResolvedGraph = std::make_shared<Serialize::ObjectGraphDocument>(
                BuildRuntimeResolvedGraph(result.artifact, &runtimeResolvedGraphStats));
            result.artifact.runtimeResolvedGraph = std::move(runtimeResolvedGraph);
            result.artifact.runtimeResolvedGraphFingerprint = runtimeResolvedGraphFingerprint;
            resolveScope.AddCounter("dependencyCount", result.artifact.resolvedAssets.size());
            resolveScope.AddCounter("runtimeResolvedGraphPrimedCount", 1u);
            resolveScope.AddCounter("runtimeResolvedGraphCopyCount", 1u);
            AddRuntimeResolvedGraphStatsCounters(resolveScope, runtimeResolvedGraphStats);
        }
    }
    else
    {
        result.diagnostics = result.artifact.Validate(runtimeResolvedGraphFingerprint);
        if (!result.diagnostics.HasErrors() && options.trustResolvedAssets && !result.artifact.resolvedAssets.empty())
        {
            NLS::Base::Profiling::PerformanceStageScope resolveScope(
                NLS::Base::Profiling::PerformanceStageDomain::Prefab,
                "ResolveExternalReferences",
                NLS::Base::Profiling::PerformanceStageThread::Main);
            RuntimeResolvedGraphStats runtimeResolvedGraphStats;
            auto runtimeResolvedGraph = std::make_shared<Serialize::ObjectGraphDocument>(
                BuildRuntimeResolvedGraph(result.artifact, &runtimeResolvedGraphStats));
            result.artifact.runtimeResolvedGraph = std::move(runtimeResolvedGraph);
            result.artifact.runtimeResolvedGraphFingerprint = runtimeResolvedGraphFingerprint;
            resolveScope.AddCounter("dependencyCount", result.artifact.resolvedAssets.size());
            resolveScope.AddCounter("runtimeResolvedGraphPrimedAfterValidationCount", 1u);
            resolveScope.AddCounter("runtimeResolvedGraphCopyCount", 1u);
            AddRuntimeResolvedGraphStatsCounters(resolveScope, runtimeResolvedGraphStats);
        }
    }
    return result;
}

PrefabArtifactInstantiationResult InstantiatePrefabArtifact(
    PrefabArtifact& artifact,
    SceneSystem::Scene& scene)
{
    return InstantiatePrefabArtifact(artifact, scene, {});
}

PrefabArtifactInstantiationResult InstantiatePrefabArtifact(
    const PrefabArtifact& artifact,
    SceneSystem::Scene& scene)
{
    return InstantiatePrefabArtifact(artifact, scene, {});
}

PrefabArtifactInstantiationResult InstantiatePrefabArtifact(
    PrefabArtifact& artifact,
    SceneSystem::Scene& scene,
    const Serialize::LoadPolicy& policy)
{
    auto result = InstantiatePrefabArtifact(
        static_cast<const PrefabArtifact&>(artifact),
        scene,
        policy);
    if (!result.diagnostics.HasErrors())
        artifact.sourceToRuntimeObject = result.sourceToInstance;
    return result;
}

PrefabArtifactInstantiationResult InstantiatePrefabArtifact(
    const PrefabArtifact& artifact,
    SceneSystem::Scene& scene,
    const Serialize::LoadPolicy& policy)
{
    NLS::Base::Profiling::PerformanceStageScope totalScope(
        NLS::Base::Profiling::PerformanceStageDomain::Prefab,
        "TotalInstantiate",
        NLS::Base::Profiling::PerformanceStageThread::Main);
    totalScope.AddCounter("objectCount", artifact.graph.objects.size());
    totalScope.AddCounter("dependencyCount", artifact.resolvedAssets.size());
    const auto artifactFingerprint = BuildRuntimeResolvedGraphFingerprint(artifact);

    PrefabArtifactInstantiationResult result;
    result.diagnostics = artifact.Validate(artifactFingerprint);
    if (result.diagnostics.HasErrors())
        return result;

    if (policy.synchronousAssetReferencePrewarm)
    {
        NLS::Base::Profiling::PerformanceStageScope waitScope(
            NLS::Base::Profiling::PerformanceStageDomain::Prefab,
            "WaitForResources",
            NLS::Base::Profiling::PerformanceStageThread::Main);
        NLS::Base::Profiling::PerformanceStageScope uploadScope(
            NLS::Base::Profiling::PerformanceStageDomain::Prefab,
            "UploadGpuResources",
            NLS::Base::Profiling::PerformanceStageThread::Main);
        uploadScope.AddCounter("dependencyCount", artifact.resolvedAssets.size());
        PrewarmPrefabMeshArtifacts(artifact, &uploadScope);
        PrewarmPrefabMaterialArtifacts(artifact, &uploadScope);
    }

    const bool needsRuntimeResolvedGraph = !artifact.resolvedAssets.empty();
    const bool needsPrefabDocument = needsRuntimeResolvedGraph || !artifact.graph.overrides.empty();
    Serialize::PrefabDocument document;
    std::shared_ptr<const Serialize::ObjectGraphDocument> runtimeResolvedGraph;
    uint64_t runtimeResolvedGraphCopyCount = 0u;
    uint64_t runtimeResolvedGraphCacheHitCount = 0u;
    {
        NLS::Base::Profiling::PerformanceStageScope resolveScope(
            NLS::Base::Profiling::PerformanceStageDomain::Prefab,
            "ResolveExternalReferences",
            NLS::Base::Profiling::PerformanceStageThread::Main);
        RuntimeResolvedGraphStats runtimeResolvedGraphStats;
        if (needsRuntimeResolvedGraph)
        {
            bool cacheHit = false;
            runtimeResolvedGraph = GetOrBuildRuntimeResolvedGraph(
                artifact,
                artifactFingerprint,
                cacheHit,
                &runtimeResolvedGraphStats);
            runtimeResolvedGraphCopyCount = cacheHit ? 0u : 1u;
            runtimeResolvedGraphCacheHitCount = cacheHit ? 1u : 0u;
        }
        else if (needsPrefabDocument)
            document.graph = artifact.graph;
        resolveScope.AddCounter("dependencyCount", artifact.resolvedAssets.size());
        resolveScope.AddCounter("runtimeResolvedGraphCopyCount", runtimeResolvedGraphCopyCount);
        if (runtimeResolvedGraphCacheHitCount > 0u)
            resolveScope.AddCounter("runtimeResolvedGraphCacheHitCount", runtimeResolvedGraphCacheHitCount);
        if (runtimeResolvedGraphCopyCount > 0u)
            AddRuntimeResolvedGraphStatsCounters(resolveScope, runtimeResolvedGraphStats);
    }
    std::shared_ptr<const Serialize::ObjectGraphInstantiator::PrefabInstantiatePlan> instantiatePlan;
    {
        NLS::Base::Profiling::PerformanceStageScope planScope(
            NLS::Base::Profiling::PerformanceStageDomain::Prefab,
            "PrepareInstantiatePlan",
            NLS::Base::Profiling::PerformanceStageThread::Main);

        const Serialize::ObjectGraphDocument* planGraph = nullptr;
        if (runtimeResolvedGraph)
        {
            if (runtimeResolvedGraph->overrides.empty())
                planGraph = runtimeResolvedGraph.get();
        }
        else if (artifact.graph.overrides.empty())
        {
            planGraph = &artifact.graph;
        }

        uint64_t instantiatePlanBuildCount = 0u;
        uint64_t instantiatePlanCacheHitCount = 0u;
        if (planGraph != nullptr)
        {
            bool cacheHit = false;
            instantiatePlan = GetOrBuildPrefabInstantiatePlan(
                artifact,
                *planGraph,
                artifactFingerprint,
                cacheHit);
            instantiatePlanBuildCount = cacheHit ? 0u : 1u;
            instantiatePlanCacheHitCount = cacheHit ? 1u : 0u;
        }

        planScope.AddCounter("instantiatePlanBuildCount", instantiatePlanBuildCount);
        planScope.AddCounter("instantiatePlanCacheHitCount", instantiatePlanCacheHitCount);
        if (instantiatePlan)
        {
            planScope.AddCounter("instantiatePlanGameObjectCount", instantiatePlan->gameObjects.size());
            planScope.AddCounter("instantiatePlanComponentCount", instantiatePlan->componentRecordCount);
            planScope.AddCounter(
                "instantiatePlanAssetReferenceBindingCandidateCount",
                instantiatePlan->assetReferenceBindingCandidateCount);
        }
    }
    const auto instantiated = [&]()
    {
        if (runtimeResolvedGraph)
        {
            if (runtimeResolvedGraph->overrides.empty())
            {
                return Serialize::ObjectGraphInstantiator::InstantiatePrefabGraph(
                    *runtimeResolvedGraph,
                    scene,
                    policy,
                    instantiatePlan.get());
            }

            document.graph = *runtimeResolvedGraph;
            return Serialize::ObjectGraphInstantiator::InstantiatePrefab(document, scene, policy);
        }

        return needsPrefabDocument
            ? Serialize::ObjectGraphInstantiator::InstantiatePrefab(document, scene, policy)
            : Serialize::ObjectGraphInstantiator::InstantiatePrefabGraph(
                artifact.graph,
                scene,
                policy,
                instantiatePlan.get());
    }();
    for (const auto& diagnostic : instantiated.diagnostics.GetItems())
        result.diagnostics.Add(diagnostic);
    if (result.diagnostics.HasErrors())
        return result;

    result.root = instantiated.root;
    result.sourceToInstance = instantiated.sourceToInstance;
    result.sourceByInstanceObject = instantiated.sourceByInstanceObject;

    if (!result.root)
    {
        AddDiagnostic(
            result.diagnostics,
            Serialize::SerializationDiagnosticCode::MissingObject,
            Serialize::SerializationDiagnosticSeverity::Error,
            "Prefab artifact could not be instantiated.");
    }

    return result;
}

Serialize::SerializationDiagnosticList ValidatePrefabBaseChains(
    const std::vector<PrefabArtifact>& artifacts)
{
    Serialize::SerializationDiagnosticList diagnostics;
    std::unordered_map<NLS::Core::Assets::AssetId, const PrefabArtifact*> artifactsById;
    for (const auto& artifact : artifacts)
    {
        if (artifact.assetId.IsValid())
            artifactsById.emplace(artifact.assetId, &artifact);
    }

    std::unordered_set<NLS::Core::Assets::AssetId> visited;
    for (const auto& artifact : artifacts)
    {
        std::unordered_set<NLS::Core::Assets::AssetId> visiting;
        VisitBaseChain(artifact, artifactsById, visiting, visited, diagnostics);
    }
    return diagnostics;
}

std::vector<Serialize::PatchOperation> NormalizePrefabOverridePatches(
    const std::vector<Serialize::PatchOperation>& patches)
{
    std::unordered_map<std::string, Serialize::PatchOperation> latestByKey;
    for (const auto& patch : patches)
        latestByKey[PatchDedupeKey(patch)] = patch;

    std::vector<Serialize::PatchOperation> normalized;
    normalized.reserve(latestByKey.size());
    for (auto& entry : latestByKey)
        normalized.push_back(std::move(entry.second));

    std::sort(normalized.begin(), normalized.end(), IsPatchLess);
    return normalized;
}

PrefabOverridePatchKind ClassifyPrefabOverridePatch(
    const Serialize::PatchOperation& patch,
    const bool /*includeDefaultOverrides*/)
{
    if (patch.type == Serialize::PatchOperationType::ReplaceProperty &&
        (patch.property.rfind("transform.", 0u) == 0u ||
            patch.property.rfind("layer", 0u) == 0u))
    {
        return PrefabOverridePatchKind::DefaultOverride;
    }

    switch (patch.type)
    {
    case Serialize::PatchOperationType::ReplaceProperty:
        return PrefabOverridePatchKind::Property;
    case Serialize::PatchOperationType::InsertOwned:
        return patch.property == "components"
            ? PrefabOverridePatchKind::AddedComponent
            : PrefabOverridePatchKind::AddedGameObject;
    case Serialize::PatchOperationType::RemoveOwned:
        return patch.property == "components"
            ? PrefabOverridePatchKind::RemovedComponent
            : PrefabOverridePatchKind::RemovedGameObject;
    case Serialize::PatchOperationType::MoveOwned:
        return patch.property == "components"
            ? PrefabOverridePatchKind::ReorderedComponent
            : PrefabOverridePatchKind::ReorderedGameObject;
    case Serialize::PatchOperationType::AddPrefabInstance:
        return PrefabOverridePatchKind::NestedPrefab;
    case Serialize::PatchOperationType::RemoveObject:
        return PrefabOverridePatchKind::RemovedObject;
    default:
        return PrefabOverridePatchKind::Unknown;
    }
}

std::optional<Serialize::PropertyValue> ReadPrefabPropertyValue(
    const PrefabArtifact& artifact,
    const Serialize::ObjectId& object,
    const std::string& propertyPath)
{
    const auto* record = FindObjectRecord(artifact.graph, object);
    if (!record)
        return std::nullopt;

    const auto* property = FindProperty(*record, propertyPath);
    if (!property)
        return std::nullopt;

    return property->value;
}

PrefabPropertyModification BuildPrefabPropertyModification(
    const PrefabArtifact& artifact,
    const Serialize::ObjectId& sourceObject,
    const Serialize::ObjectId& instanceObject,
    std::string propertyPath,
    Serialize::PatchOperation patch,
    std::string owningPrefabLayer,
    const bool includeDefaultOverrides)
{
    PrefabPropertyModification modification;
    modification.sourceObject = sourceObject;
    modification.instanceObject = instanceObject;
    modification.propertyPath = std::move(propertyPath);
    modification.patch = std::move(patch);
    modification.owningPrefabLayer = std::move(owningPrefabLayer);
    modification.defaultOverride =
        ClassifyPrefabOverridePatch(modification.patch, includeDefaultOverrides) ==
        PrefabOverridePatchKind::DefaultOverride;

    if (modification.patch.type == Serialize::PatchOperationType::ReplaceProperty)
    {
        modification.baseValue = ReadPrefabPropertyValue(
            artifact,
            modification.patch.target,
            modification.patch.property);
        modification.localValue = modification.patch.value;
    }
    return modification;
}

std::vector<NLS::Core::Assets::AssetId> ExtractNestedPrefabDependencies(
    const PrefabArtifact& artifact)
{
    std::vector<NLS::Core::Assets::AssetId> dependencies;
    for (const auto& object : artifact.graph.objects)
    {
        for (const auto& property : object.properties)
            CollectNestedPrefabDependenciesFromValue(artifact, property.value, dependencies, property.name);
    }

    std::sort(dependencies.begin(), dependencies.end(), [](const auto& lhs, const auto& rhs)
    {
        return lhs.ToString() < rhs.ToString();
    });
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    return dependencies;
}

Serialize::SerializationDiagnosticList ValidateNestedPrefabDependencies(
    const std::vector<PrefabArtifact>& artifacts)
{
    Serialize::SerializationDiagnosticList diagnostics;
    std::unordered_map<NLS::Core::Assets::AssetId, const PrefabArtifact*> artifactsById;
    for (const auto& artifact : artifacts)
    {
        if (artifact.assetId.IsValid())
            artifactsById.emplace(artifact.assetId, &artifact);
    }

    std::unordered_set<NLS::Core::Assets::AssetId> visited;
    for (const auto& artifact : artifacts)
    {
        std::unordered_set<NLS::Core::Assets::AssetId> visiting;
        VisitNestedPrefabDependencies(artifact, artifactsById, visiting, visited, diagnostics);
    }
    return diagnostics;
}
}
