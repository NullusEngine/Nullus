#pragma once

#include "Assets/AssetId.h"
#include "EngineDef.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/ObjectGraphInstantiator.h"
#include "Serialize/SerializationDiagnostic.h"
#include "SceneSystem/Scene.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace NLS::Engine::Assets
{
struct PrefabResolvedAsset
{
    NLS::Core::Assets::AssetId assetId;
    std::string expectedType;
    std::string subAssetKey;
    std::string artifactPath;
};

struct NLS_ENGINE_API PrefabArtifact
{
    NLS::Core::Assets::AssetId assetId;
    Serialize::ObjectGraphDocument graph;
    bool generatedModelPrefab = false;
    std::vector<NLS::Core::Assets::AssetId> baseChain;
    std::vector<PrefabResolvedAsset> resolvedAssets;
    std::unordered_map<Serialize::ObjectId, Serialize::ObjectId> sourceToRuntimeObject;

    const Serialize::ObjectId* FindRuntimeObject(const Serialize::ObjectId& sourceObject) const;
    Serialize::SerializationDiagnosticList Validate() const;
};

struct PrefabImportResult
{
    PrefabArtifact artifact;
    Serialize::SerializationDiagnosticList diagnostics;
};

NLS_ENGINE_API std::vector<Serialize::ObjectIdentifier> CollectPrefabAssetReferences(
    const Serialize::ObjectGraphDocument& graph);

NLS_ENGINE_API std::string ExtractPrefabAssetReferenceSubAssetKeyHint(
    const std::string& referencePath);

NLS_ENGINE_API std::vector<PrefabResolvedAsset> BuildPrefabResolvedAssetsFromReferences(
    const Serialize::ObjectGraphDocument& graph);

NLS_ENGINE_API std::vector<PrefabResolvedAsset> BuildPrefabResolvedAssetsFromReferences(
    const Serialize::ObjectGraphDocument& graph,
    const std::vector<PrefabResolvedAsset>& existingResolvedAssets);

NLS_ENGINE_API void RefreshPrefabResolvedAssetsFromReferences(PrefabArtifact& artifact);

struct PrefabArtifactInstantiationResult
{
    NLS::Engine::GameObject* root = nullptr;
    std::unordered_map<Serialize::ObjectId, Serialize::ObjectId> sourceToInstance;
    std::unordered_map<NLS::Engine::GameObject*, Serialize::ObjectId> sourceByInstanceObject;
    Serialize::SerializationDiagnosticList diagnostics;
};

enum class PrefabOverridePatchKind
{
    Property,
    DefaultOverride,
    AddedComponent,
    RemovedComponent,
    ReorderedComponent,
    AddedGameObject,
    RemovedGameObject,
    ReorderedGameObject,
    NestedPrefab,
    RemovedObject,
    Unknown
};

struct PrefabPropertyModification
{
    Serialize::ObjectId sourceObject;
    Serialize::ObjectId instanceObject;
    std::string propertyPath;
    Serialize::PatchOperation patch;
    std::optional<Serialize::PropertyValue> baseValue;
    std::optional<Serialize::PropertyValue> localValue;
    std::string owningPrefabLayer;
    bool defaultOverride = false;
};

NLS_ENGINE_API PrefabImportResult ImportPrefabArtifact(
    const std::string& sourceText,
    NLS::Core::Assets::AssetId assetId);

NLS_ENGINE_API PrefabImportResult ImportPrefabArtifact(
    const std::string& sourceText,
    NLS::Core::Assets::AssetId assetId,
    std::vector<PrefabResolvedAsset> resolvedAssets);

NLS_ENGINE_API PrefabArtifactInstantiationResult InstantiatePrefabArtifact(
    PrefabArtifact& artifact,
    SceneSystem::Scene& scene);

NLS_ENGINE_API PrefabArtifactInstantiationResult InstantiatePrefabArtifact(
    const PrefabArtifact& artifact,
    SceneSystem::Scene& scene);

NLS_ENGINE_API PrefabArtifactInstantiationResult InstantiatePrefabArtifact(
    PrefabArtifact& artifact,
    SceneSystem::Scene& scene,
    const Serialize::LoadPolicy& policy);

NLS_ENGINE_API PrefabArtifactInstantiationResult InstantiatePrefabArtifact(
    const PrefabArtifact& artifact,
    SceneSystem::Scene& scene,
    const Serialize::LoadPolicy& policy);

NLS_ENGINE_API Serialize::SerializationDiagnosticList ValidatePrefabBaseChains(
    const std::vector<PrefabArtifact>& artifacts);

NLS_ENGINE_API std::vector<Serialize::PatchOperation> NormalizePrefabOverridePatches(
    const std::vector<Serialize::PatchOperation>& patches);

NLS_ENGINE_API PrefabOverridePatchKind ClassifyPrefabOverridePatch(
    const Serialize::PatchOperation& patch,
    bool includeDefaultOverrides);

NLS_ENGINE_API std::optional<Serialize::PropertyValue> ReadPrefabPropertyValue(
    const PrefabArtifact& artifact,
    const Serialize::ObjectId& object,
    const std::string& propertyPath);

NLS_ENGINE_API PrefabPropertyModification BuildPrefabPropertyModification(
    const PrefabArtifact& artifact,
    const Serialize::ObjectId& sourceObject,
    const Serialize::ObjectId& instanceObject,
    std::string propertyPath,
    Serialize::PatchOperation patch,
    std::string owningPrefabLayer,
    bool includeDefaultOverrides);

NLS_ENGINE_API std::vector<NLS::Core::Assets::AssetId> ExtractNestedPrefabDependencies(
    const PrefabArtifact& artifact);

NLS_ENGINE_API Serialize::SerializationDiagnosticList ValidateNestedPrefabDependencies(
    const std::vector<PrefabArtifact>& artifacts);
}
