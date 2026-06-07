#include "Engine/Assets/PrefabAsset.h"

#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ServiceLocator.h"
#include "Serialize/ObjectGraphInstantiator.h"
#include "Serialize/ObjectGraphReader.h"
#include "Serialize/PrefabDocument.h"

#include <algorithm>
#include <filesystem>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace NLS::Engine::Assets
{
namespace
{
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

bool HasResolvedAsset(
    const PrefabArtifact& artifact,
    const Serialize::ObjectIdentifier& reference)
{
    const auto assetId = NLS::Core::Assets::AssetId(reference.guid);
    const auto referencePath = std::filesystem::path(reference.filePath).lexically_normal();
    for (const auto& resolved : artifact.resolvedAssets)
    {
        const auto resolvedPath = std::filesystem::path(resolved.artifactPath).lexically_normal();
        if (resolved.assetId == assetId &&
            (reference.filePath.empty() ||
                resolved.subAssetKey == reference.filePath ||
                resolved.artifactPath == reference.filePath ||
                (!resolved.artifactPath.empty() && resolvedPath == referencePath)))
        {
            return true;
        }
    }
    return false;
}

void ValidateResolvedAssetValue(
    const PrefabArtifact& artifact,
    const Serialize::PropertyValue& value,
    Serialize::SerializationDiagnosticList& diagnostics)
{
    switch (value.GetKind())
    {
    case Serialize::PropertyValue::Kind::ObjectReference:
        if (value.GetObjectReference().guid.IsValid() &&
            !HasResolvedAsset(artifact, value.GetObjectReference()))
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
            ValidateResolvedAssetValue(artifact, item, diagnostics);
        break;
    case Serialize::PropertyValue::Kind::Object:
        for (const auto& property : value.GetObject())
            ValidateResolvedAssetValue(artifact, property.second, diagnostics);
        break;
    default:
        break;
    }
}

void ValidateResolvedAssetReferences(
    const PrefabArtifact& artifact,
    Serialize::SerializationDiagnosticList& diagnostics)
{
    for (const auto& object : artifact.graph.objects)
    {
        for (const auto& property : object.properties)
            ValidateResolvedAssetValue(artifact, property.value, diagnostics);
    }
}

void CollectAssetReferencesFromValue(
    const Serialize::PropertyValue& value,
    std::vector<Serialize::ObjectIdentifier>& references)
{
    switch (value.GetKind())
    {
    case Serialize::PropertyValue::Kind::ObjectReference:
        if (value.GetObjectReference().guid.IsValid())
            references.push_back(value.GetObjectReference());
        break;
    case Serialize::PropertyValue::Kind::Array:
        for (const auto& item : value.GetArray())
            CollectAssetReferencesFromValue(item, references);
        break;
    case Serialize::PropertyValue::Kind::Object:
        for (const auto& property : value.GetObject())
            CollectAssetReferencesFromValue(property.second, references);
        break;
    default:
        break;
    }
}

bool EndsWith(std::string_view value, std::string_view suffix)
{
    return value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string InferResolvedAssetType(const Serialize::ObjectIdentifier& reference)
{
    const auto& path = reference.filePath;
    if (path.rfind("mesh:", 0) == 0 || EndsWith(path, ".nmesh"))
        return "Mesh";
    if (path.rfind("material:", 0) == 0 || EndsWith(path, ".nmat"))
        return "Material";
    if (path.rfind("texture:", 0) == 0 || EndsWith(path, ".ntex"))
        return "Texture";
    if (path.rfind("shader:", 0) == 0 || EndsWith(path, ".hlsl") || EndsWith(path, ".shader"))
        return "Shader";
    if (path.rfind("prefab:", 0) == 0 || EndsWith(path, ".prefab"))
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

    const auto referencePath = std::filesystem::path(reference.filePath).lexically_normal();
    const auto resolvedPath = std::filesystem::path(resolved.artifactPath).lexically_normal();
    return resolved.subAssetKey == reference.filePath ||
        resolved.artifactPath == reference.filePath ||
        (!resolved.artifactPath.empty() && resolvedPath == referencePath);
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

PrefabResolvedAsset BuildFallbackResolvedAsset(const Serialize::ObjectIdentifier& reference)
{
    PrefabResolvedAsset resolved;
    resolved.assetId = NLS::Core::Assets::AssetId(reference.guid);
    resolved.expectedType = InferResolvedAssetType(reference);
    resolved.subAssetKey = reference.filePath;
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
                    resolved.subAssetKey == reference.filePath ||
                    resolved.artifactPath == reference.filePath);
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

const PrefabResolvedAsset* FindResolvedAsset(
    const PrefabArtifact& artifact,
    const Serialize::ObjectIdentifier& reference)
{
    const auto assetId = NLS::Core::Assets::AssetId(reference.guid);
    const PrefabResolvedAsset* candidate = nullptr;
    for (const auto& resolved : artifact.resolvedAssets)
    {
        if (!reference.filePath.empty())
        {
            if ((resolved.assetId == assetId || resolved.subAssetKey == reference.filePath) &&
                (resolved.subAssetKey == reference.filePath || resolved.artifactPath == reference.filePath))
            {
                return &resolved;
            }
            continue;
        }

        if (resolved.assetId != assetId)
            continue;

        if (candidate)
            return nullptr;
        candidate = &resolved;
    }
    return candidate;
}

Serialize::PropertyValue ResolveObjectIdentifier(
    const PrefabArtifact& artifact,
    const Serialize::PropertyValue& value)
{
    switch (value.GetKind())
    {
    case Serialize::PropertyValue::Kind::ObjectReference:
    {
        auto reference = value.GetObjectReference();
        if (!reference.guid.IsValid())
            return value;
        if (const auto* resolved = FindResolvedAsset(artifact, reference);
            resolved && !resolved->artifactPath.empty())
        {
            reference.filePath = resolved->artifactPath;
        }
        return Serialize::PropertyValue::ObjectReference(std::move(reference));
    }
    case Serialize::PropertyValue::Kind::Array:
    {
        Serialize::PropertyValue::ArrayValue values;
        values.reserve(value.GetArray().size());
        for (const auto& item : value.GetArray())
            values.push_back(ResolveObjectIdentifier(artifact, item));
        return Serialize::PropertyValue::Array(std::move(values));
    }
    case Serialize::PropertyValue::Kind::Object:
    {
        Serialize::PropertyValue::ObjectValue properties;
        properties.reserve(value.GetObject().size());
        for (const auto& property : value.GetObject())
            properties.push_back({property.first, ResolveObjectIdentifier(artifact, property.second)});
        return Serialize::PropertyValue::Object(std::move(properties));
    }
    default:
        return value;
    }
}

Serialize::ObjectGraphDocument BuildRuntimeResolvedGraph(const PrefabArtifact& artifact)
{
    auto graph = artifact.graph;
    for (auto& object : graph.objects)
    {
        for (auto& property : object.properties)
            property.value = ResolveObjectIdentifier(artifact, property.value);
    }
    return graph;
}

void PrewarmPrefabMeshArtifacts(const PrefabArtifact& artifact)
{
    if (!Core::ServiceLocator::Contains<Core::ResourceManagement::MeshManager>())
        return;

    auto& meshManager = NLS_SERVICE(Core::ResourceManagement::MeshManager);
    for (const auto& resolved : artifact.resolvedAssets)
    {
        if (resolved.expectedType == "Mesh" && !resolved.artifactPath.empty())
            meshManager.PrewarmArtifact(resolved.artifactPath);
    }
}

void PrewarmPrefabMaterialArtifacts(const PrefabArtifact& artifact)
{
    if (!Core::ServiceLocator::Contains<Core::ResourceManagement::MaterialManager>())
        return;

    auto& materialManager = NLS_SERVICE(Core::ResourceManagement::MaterialManager);
    for (const auto& resolved : artifact.resolvedAssets)
    {
        if (resolved.expectedType == "Material" && !resolved.artifactPath.empty())
            materialManager.LoadArtifactWithoutTextures(resolved.artifactPath);
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
    Serialize::PrefabDocument prefab;
    prefab.graph = graph;
    auto diagnostics = Serialize::ObjectGraphInstantiator::ValidatePrefab(prefab);
    ValidateResolvedAssetReferences(*this, diagnostics);
    return diagnostics;
}

std::vector<Serialize::ObjectIdentifier> CollectPrefabAssetReferences(
    const Serialize::ObjectGraphDocument& graph)
{
    std::vector<Serialize::ObjectIdentifier> references;
    for (const auto& object : graph.objects)
    {
        for (const auto& property : object.properties)
            CollectAssetReferencesFromValue(property.value, references);
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
    for (const auto& reference : CollectPrefabAssetReferences(graph))
    {
        auto resolved = FindExistingResolvedAssetForReference(existingResolvedAssets, reference);
        auto resolvedAsset = resolved.has_value()
            ? std::move(*resolved)
            : BuildFallbackResolvedAsset(reference);
        if (!ContainsResolvedAssetReference(resolvedAssets, resolvedAsset))
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

    result.artifact.graph = *document;
    RefreshPrefabResolvedAssetsFromReferences(result.artifact);
    if (document->basePrefab.has_value())
    {
        result.artifact.baseChain.push_back(
            NLS::Core::Assets::AssetId(document->basePrefab->guid));
    }

    result.diagnostics = result.artifact.Validate();
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
    PrefabArtifactInstantiationResult result;
    result.diagnostics = artifact.Validate();
    if (result.diagnostics.HasErrors())
        return result;

    if (!policy.deferAssetReferenceResolution)
    {
        PrewarmPrefabMeshArtifacts(artifact);
        PrewarmPrefabMaterialArtifacts(artifact);
    }

    Serialize::PrefabDocument document;
    document.graph = BuildRuntimeResolvedGraph(artifact);
    const auto instantiated = Serialize::ObjectGraphInstantiator::InstantiatePrefab(document, scene, policy);
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
