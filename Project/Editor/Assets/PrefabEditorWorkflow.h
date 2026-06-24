#pragma once

#include "Assets/AssetDiagnostics.h"
#include "Assets/AssetId.h"
#include "Assets/AssetVersion.h"
#include "Engine/Assets/PrefabAsset.h"
#include "GameObject.h"
#include "SceneSystem/Scene.h"
#include "Serialize/ObjectGraphDocument.h"

#include <filesystem>
#include <functional>
#include <initializer_list>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace NLS::Editor::Assets
{
enum class PrefabEditorOperationStatus
{
    Rejected,
    Failed,
    Committed
};

struct PrefabEditorDiagnostic
{
    std::string code;
    std::string message;
};

struct PrefabOverrideRecord
{
    NLS::Engine::Serialize::ObjectId sourceObject;
    NLS::Engine::Serialize::ObjectId instanceObject;
    std::string propertyPath;
    NLS::Engine::Serialize::PatchOperation patch;
    std::optional<NLS::Engine::Serialize::ObjectRecord> objectRecord;
    std::vector<NLS::Engine::Serialize::ObjectRecord> objectRecords;
    std::string owningPrefabLayer;
};

struct PrefabStageState
{
    NLS::Core::Assets::AssetId prefabAssetId;
    std::string prefabAssetPath;
    std::string prefabSubAssetKey;
    bool loaded = false;
    bool dirty = false;
    bool editable = true;
    bool generatedReadOnly = false;
    NLS::Engine::GameObject* stageRoot = nullptr;
    NLS::Engine::Serialize::ObjectGraphDocument openedGraph;
    std::unique_ptr<NLS::Engine::SceneSystem::Scene> stageScene;
};

struct OpenPrefabStageRequest
{
    NLS::Engine::Assets::PrefabArtifact* prefab = nullptr;
    NLS::Core::Assets::AssetId prefabAssetId;
    std::string prefabSubAssetKey;
    bool generatedReadOnly = false;
    std::string prefabAssetPath;
};

struct PrefabVariantRequest
{
    NLS::Core::Assets::AssetId basePrefabAssetId;
    std::string basePrefabSubAssetKey;
    std::filesystem::path destinationPath;
    NLS::Core::Assets::AssetId newVariantAssetId;
};

struct CreateEditableVariantRequest
{
    NLS::Engine::Assets::PrefabArtifact* basePrefab = nullptr;
    NLS::Core::Assets::AssetId basePrefabAssetId;
    std::string basePrefabSubAssetKey;
    std::filesystem::path destinationPath;
    NLS::Core::Assets::AssetId newVariantAssetId;
    bool baseIsGeneratedReadOnly = false;
    bool destinationExists = false;
};

struct PrefabUnpackResult
{
    NLS::Engine::GameObject* root = nullptr;
    std::vector<NLS::Engine::Serialize::ObjectId> sceneOwnedObjects;
    std::vector<NLS::Engine::Serialize::ObjectIdentifier> preservedAssetReferences;
};

struct PrefabInstanceRecord
{
    NLS::Core::Assets::AssetId prefabAssetId;
    NLS::Core::Assets::AssetId sceneAssetId;
    std::string prefabSubAssetKey;
    bool generatedReadOnly = false;
    bool unpacked = false;
    NLS::Engine::GameObject* instanceRoot = nullptr;
    NLS::Engine::Serialize::ObjectGraphDocument sourceGraph;
    std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact> sharedSourcePrefab;
    std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::Serialize::ObjectId> sourceToInstance;
    std::unordered_map<const NLS::Engine::GameObject*, NLS::Engine::Serialize::ObjectId> sourceByInstanceObject;
    std::vector<NLS::Engine::Serialize::PatchOperation> localPatches;
    NLS::Engine::Serialize::ObjectId preservedInstanceRootObject;
    std::vector<NLS::Engine::Serialize::ObjectRecord> preservedAddedObjects;
    std::vector<NLS::Engine::Serialize::PrefabInstanceObjectCorrespondence> preservedCorrespondence;
    std::vector<NLS::Engine::Serialize::ObjectIdentifier> preservedAssetReferences;
    std::vector<NLS::Engine::Assets::PrefabResolvedAsset> preservedResolvedAssets;
    std::vector<PrefabInstanceRecord> nestedInstances;

    const NLS::Engine::Serialize::ObjectGraphDocument& SourceGraph() const;
    const NLS::Engine::Assets::PrefabArtifact* SharedSourcePrefab() const;
    void UseSharedSourcePrefab(std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact> prefab);
};

enum class PrefabHierarchyState
{
    None,
    Root,
    Child
};

enum class PrefabHierarchyColorToken
{
    Default,
    ConnectedRoot,
    ConnectedChild,
    Override,
    Missing,
    GeneratedReadOnly,
    Pending,
    Unpacked
};

struct PrefabHierarchyPresentation
{
    PrefabHierarchyState state = PrefabHierarchyState::None;
    PrefabHierarchyColorToken color = PrefabHierarchyColorToken::Default;
    NLS::Core::Assets::AssetId assetId;
    std::string subAssetKey;
    bool hasOverrides = false;
    bool missingAsset = false;
    bool generatedReadOnly = false;
    bool pendingResources = false;
    bool unpacked = false;
};

class PrefabInstanceRegistry
{
public:
    PrefabInstanceRecord& Register(PrefabInstanceRecord instance);
    void Clear();
    PrefabInstanceRecord* FindRootInstance(const NLS::Engine::GameObject& object);
    const PrefabInstanceRecord* FindRootInstance(const NLS::Engine::GameObject& object) const;
    bool RemoveRootInstance(const NLS::Engine::GameObject& object);
    bool RemoveObjectMapping(const NLS::Engine::GameObject& object);
    void MarkAssetMissing(NLS::Core::Assets::AssetId assetId, bool missing);
    void MarkAssetMissing(NLS::Core::Assets::AssetId assetId, const std::string& prefabSubAssetKey, bool missing);
    void MarkAssetPendingResources(NLS::Core::Assets::AssetId assetId, bool pending);
    void MarkAssetPendingResources(NLS::Core::Assets::AssetId assetId, const std::string& prefabSubAssetKey, bool pending);
    void MarkInstanceResourceFailure(const NLS::Engine::GameObject& instanceRoot, bool failed);
    void MarkInstancePendingResources(const NLS::Engine::GameObject& instanceRoot, bool pending);
    void ClearInstancePendingResourcesForPrefab(NLS::Core::Assets::AssetId assetId);
    PrefabInstanceRecord* FindInstance(const NLS::Engine::GameObject& object);
    const PrefabInstanceRecord* FindInstance(const NLS::Engine::GameObject& object) const;
    const std::deque<PrefabInstanceRecord>& GetInstances() const { return m_instances; }
    std::vector<PrefabInstanceRecord*> FindInstancesForPrefab(
        NLS::Core::Assets::AssetId prefabAssetId,
        const std::string& prefabSubAssetKey = {});
    PrefabHierarchyPresentation GetPresentation(const NLS::Engine::GameObject& object) const;

private:
    std::deque<PrefabInstanceRecord> m_instances;
    std::unordered_set<std::string> m_missingPrefabSources;
    std::unordered_set<std::string> m_pendingResourcePrefabSources;
    std::unordered_set<const NLS::Engine::GameObject*> m_failedResourceInstanceRoots;
    std::unordered_set<const NLS::Engine::GameObject*> m_pendingResourceInstanceRoots;
};

struct CreatePrefabFromSelectionRequest
{
    NLS::Engine::GameObject* selectedRoot = nullptr;
    std::vector<NLS::Engine::GameObject*> selectedRoots;
    NLS::Core::Assets::AssetId destinationAssetId;
    std::filesystem::path destinationPath;
};

struct InstantiatePrefabRequest
{
    NLS::Engine::Assets::PrefabArtifact* prefab = nullptr;
    NLS::Core::Assets::AssetId prefabAssetId;
    std::string prefabSubAssetKey;
    NLS::Core::Assets::AssetId sceneAssetId;
    bool deferAssetReferenceResolution = false;
    const NLS::Engine::Assets::PrefabArtifact* constPrefab = nullptr;
    bool synchronousAssetReferencePrewarm = false;
};

struct PrefabEditorOperationResult
{
    PrefabEditorOperationStatus status = PrefabEditorOperationStatus::Failed;
    std::vector<PrefabEditorDiagnostic> diagnostics;
    std::optional<NLS::Engine::Assets::PrefabArtifact> artifact;
    std::optional<PrefabInstanceRecord> instance;
    std::optional<PrefabStageState> stage;
    std::optional<PrefabUnpackResult> unpack;
    NLS::Core::Assets::AssetId createdPrefabAssetId;
    std::filesystem::path createdPrefabPath;
    std::string prefabSourceText;
    std::vector<NLS::Core::Assets::AssetDependencyChange> dependencyChanges;
    std::vector<NLS::Core::Assets::AssetDependencyRecord> dependencyRefreshRequests;
};

class PrefabEditorWorkflow
{
public:
    PrefabEditorOperationResult CreatePrefabFromSelection(const CreatePrefabFromSelectionRequest& request) const;
    PrefabEditorOperationResult InstantiatePrefab(
        const InstantiatePrefabRequest& request,
        NLS::Engine::SceneSystem::Scene& scene) const;
    PrefabEditorOperationResult ConnectExistingPrefabInstance(
        const InstantiatePrefabRequest& request,
        NLS::Engine::GameObject& root) const;
    std::vector<PrefabOverrideRecord> DiscoverOverrides(
        const NLS::Engine::Assets::PrefabArtifact& prefab,
        const PrefabInstanceRecord& instance) const;
    PrefabEditorOperationResult ApplySelectedOverride(
        NLS::Engine::Assets::PrefabArtifact& prefab,
        const PrefabOverrideRecord& overrideRecord) const;
    PrefabEditorOperationResult ApplyAllOverrides(
        NLS::Engine::Assets::PrefabArtifact& prefab,
        const std::vector<PrefabOverrideRecord>& overrides) const;
    PrefabEditorOperationResult RevertSelectedOverride(
        PrefabInstanceRecord& instance,
        const NLS::Engine::Serialize::PatchOperation& patch) const;
    PrefabEditorOperationResult RevertAllOverrides(PrefabInstanceRecord& instance) const;
    PrefabEditorOperationResult OpenPrefabStage(const OpenPrefabStageRequest& request) const;
    void MarkStageDirty(PrefabStageState& stage) const;
    PrefabEditorOperationResult SavePrefabStage(
        PrefabStageState& stage,
        NLS::Engine::Assets::PrefabArtifact& prefab,
        PrefabInstanceRegistry* instanceRegistry = nullptr) const;
    PrefabEditorOperationResult DiscardPrefabStage(PrefabStageState& stage) const;
    PrefabEditorOperationResult CreateEditableVariant(const CreateEditableVariantRequest& request) const;
    PrefabEditorOperationResult ValidateNestedPrefabs(
        const std::vector<NLS::Engine::Assets::PrefabArtifact>& prefabs) const;
    PrefabEditorOperationResult UnpackPrefabInstance(PrefabInstanceRecord& instance) const;
    PrefabEditorOperationResult AggregatePrefabEditorDiagnostics(
        std::initializer_list<std::reference_wrapper<const PrefabEditorOperationResult>> operationResults,
        const std::vector<NLS::Engine::Assets::PrefabArtifact>& artifacts) const;
};
}
