#pragma once

#include "Assets/AssetMeta.h"
#include "Assets/PrefabEditorWorkflow.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace NLS::Editor::Assets
{
enum class PrefabAssetType
{
    NotAPrefab,
    Regular,
    Model,
    Variant,
    MissingAsset,
    Corrupt
};

enum class PrefabInstanceStatus
{
    NotAPrefab,
    Connected,
    Disconnected,
    MissingAsset,
    Invalid
};

enum class PrefabOverrideKind
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
    Unknown
};

enum class PrefabUnpackMode
{
    OutermostRoot,
    Completely
};

enum class PrefabOperationStatus
{
    Rejected,
    Failed,
    Committed
};

enum class PrefabEditorConnectionState
{
    NotAPrefab,
    Connected,
    MissingSource,
    Disconnected,
    Unpacked,
    Pending,
    Invalid
};

enum class PrefabEditorApplyAvailability
{
    Allowed,
    ReadOnlyRejected,
    Unsupported,
    Unavailable
};

enum class PrefabEditorResourceState
{
    Ready,
    Pending,
    Failed,
    Cancelled
};

struct PrefabDiagnostic
{
    std::string code;
    std::string message;
};

struct PrefabAssetQuery
{
    const NLS::Engine::Assets::PrefabArtifact* prefab = nullptr;
    bool generatedModelPrefab = false;
    bool missingAsset = false;
    bool corrupt = false;
    std::string prefabSubAssetKey;
};

struct PrefabInstanceQuery
{
    const PrefabInstanceRecord* instance = nullptr;
    bool assetExists = true;
    bool corrupt = false;
};

struct LoadPrefabContentsRequest
{
    NLS::Engine::Assets::PrefabArtifact* prefab = nullptr;
    NLS::Core::Assets::AssetId prefabAssetId;
    std::string prefabSubAssetKey;
    bool generatedReadOnly = false;
    std::string prefabAssetPath;
};

struct PrefabSourceIdentity
{
    std::string projectRootId;
    NLS::Core::Assets::AssetId sourceAssetId;
    std::string sourceAssetPath;
    std::string prefabSubAssetKey;
    NLS::Core::Assets::AssetType assetType = NLS::Core::Assets::AssetType::Unknown;
    std::string importerId;
    uint32_t importerVersion = 0u;

    bool operator==(const PrefabSourceIdentity& other) const
    {
        return projectRootId == other.projectRootId &&
            sourceAssetId == other.sourceAssetId &&
            sourceAssetPath == other.sourceAssetPath &&
            prefabSubAssetKey == other.prefabSubAssetKey &&
            assetType == other.assetType &&
            importerId == other.importerId &&
            importerVersion == other.importerVersion;
    }
};

PrefabSourceIdentity NormalizePrefabSourceIdentity(
    const std::filesystem::path& projectRoot,
    const std::string& sourceAssetPath,
    const std::string& prefabSubAssetKey,
    NLS::Core::Assets::AssetId sourceAssetId = {},
    NLS::Core::Assets::AssetType assetType = NLS::Core::Assets::AssetType::Unknown);

struct PrefabInstantiateRequest
{
    NLS::Engine::Assets::PrefabArtifact* prefab = nullptr;
    NLS::Core::Assets::AssetId prefabAssetId;
    std::string prefabSubAssetKey;
    NLS::Core::Assets::AssetId sceneAssetId;
    bool deferAssetReferenceResolution = false;
    const NLS::Engine::Assets::PrefabArtifact* constPrefab = nullptr;
    bool synchronousAssetReferencePrewarm = false;
};

struct PrefabOverrideDescriptor
{
    PrefabOverrideKind kind = PrefabOverrideKind::Unknown;
    NLS::Engine::Serialize::ObjectId sourceObject;
    NLS::Engine::Serialize::ObjectId instanceObject;
    std::string propertyPath;
    NLS::Engine::Serialize::PatchOperation patch;
    std::optional<NLS::Engine::Serialize::PropertyValue> baseValue;
    std::optional<NLS::Engine::Serialize::PropertyValue> localValue;
    std::optional<NLS::Engine::Serialize::ObjectRecord> objectRecord;
    std::vector<NLS::Engine::Serialize::ObjectRecord> objectRecords;
    std::string owningPrefabLayer;
    bool canApply = false;
    bool canRevert = false;
    bool defaultOverride = false;
};

struct MissingPrefabRecoveryRecord
{
    NLS::Core::Assets::AssetId missingPrefabAssetId;
    std::string missingPrefabSubAssetKey;
    NLS::Engine::GameObject* preservedInstanceRoot = nullptr;
    std::vector<NLS::Engine::Serialize::PatchOperation> preservedOverrides;
    std::vector<PrefabOverrideDescriptor> preservedOverrideRecords;
    std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::Serialize::ObjectId> preservedSourceToInstance;
    std::unordered_map<const NLS::Engine::GameObject*, NLS::Engine::Serialize::ObjectId> preservedSourceByInstanceObject;
};

struct PrefabOperationResult
{
    PrefabOperationStatus status = PrefabOperationStatus::Failed;
    std::vector<PrefabDiagnostic> diagnostics;
    std::optional<NLS::Engine::Assets::PrefabArtifact> artifact;
    std::optional<PrefabInstanceRecord> instance;
    std::optional<PrefabStageState> stage;
    std::optional<PrefabUnpackResult> unpack;
    NLS::Core::Assets::AssetId createdPrefabAssetId;
    std::filesystem::path createdPrefabPath;
    std::string prefabSourceText;
    std::vector<NLS::Core::Assets::AssetDependencyChange> dependencyChanges;
    std::vector<NLS::Core::Assets::AssetDependencyRecord> dependencyRefreshRequests;
    std::vector<NLS::Engine::Serialize::ObjectIdentifier> preservedNestedPrefabLinks;
    std::vector<NLS::Engine::Serialize::ObjectIdentifier> detachedNestedPrefabLinks;
};

struct PrefabApplyTarget
{
    NLS::Engine::Assets::PrefabArtifact* editablePrefab = nullptr;
    std::string prefabLayer;
    bool rejected = false;
    std::vector<PrefabDiagnostic> diagnostics;
};

struct PrefabEditorStateQuery
{
    const PrefabInstanceRecord* instance = nullptr;
    const NLS::Engine::Assets::PrefabArtifact* prefab = nullptr;
    bool sourceAssetExists = false;
    bool sourceCorrupt = false;
    bool resourcesPending = false;
    bool resourcesFailed = false;
    bool resourcesCancelled = false;
    bool unpacked = false;
    bool editableSourceArtifactContext = false;
    bool includeDefaultOverrides = true;
};

struct PrefabEditorState
{
    PrefabEditorConnectionState connectionState = PrefabEditorConnectionState::NotAPrefab;
    PrefabEditorApplyAvailability applyAvailability = PrefabEditorApplyAvailability::Unavailable;
    PrefabEditorResourceState resourceState = PrefabEditorResourceState::Ready;
    std::vector<PrefabDiagnostic> diagnostics;
    size_t overrideCount = 0u;
    bool hasOverrides = false;
    bool canRevert = false;
    bool generatedReadOnly = false;
    bool missingSource = false;
    bool pendingResources = false;
};

class PrefabUtilityFacade
{
public:
    PrefabAssetType GetPrefabAssetType(const PrefabAssetQuery& query) const;
    PrefabInstanceStatus GetPrefabInstanceStatus(const PrefabInstanceQuery& query) const;
    PrefabEditorState GetPrefabEditorState(const PrefabEditorStateQuery& query) const;

    PrefabOperationResult SaveAsPrefabAsset(
        NLS::Engine::GameObject& root,
        NLS::Core::Assets::AssetId destinationAssetId,
        std::filesystem::path destinationPath) const;
    PrefabOperationResult SaveAsPrefabAssetAndConnect(
        NLS::Engine::GameObject& root,
        NLS::Core::Assets::AssetId destinationAssetId,
        std::filesystem::path destinationPath,
        NLS::Engine::SceneSystem::Scene& scene,
        NLS::Core::Assets::AssetId sceneAssetId) const;
    PrefabOperationResult InstantiatePrefab(
        const PrefabInstantiateRequest& request,
        NLS::Engine::SceneSystem::Scene& scene) const;

    PrefabOperationResult LoadPrefabContents(const LoadPrefabContentsRequest& request) const;
    void MarkPrefabContentsDirty(PrefabStageState& stage) const;
    PrefabOperationResult SavePrefabContents(
        PrefabStageState& stage,
        NLS::Engine::Assets::PrefabArtifact& prefab,
        PrefabInstanceRegistry* instanceRegistry = nullptr) const;
    PrefabOperationResult UnloadPrefabContents(
        PrefabStageState& stage,
        bool saveBeforeUnload,
        NLS::Engine::Assets::PrefabArtifact* prefab = nullptr) const;

    PrefabOperationResult CreateVariant(const CreateEditableVariantRequest& request) const;
    std::vector<PrefabOverrideDescriptor> GetPrefabOverrides(
        const NLS::Engine::Assets::PrefabArtifact& prefab,
        const PrefabInstanceRecord& instance,
        bool includeDefaultOverrides) const;
    PrefabOperationResult ApplySingleOverride(
        NLS::Engine::Assets::PrefabArtifact& prefab,
        const PrefabOverrideDescriptor& overrideRecord,
        bool targetGeneratedReadOnly = false) const;
    PrefabOperationResult ApplyOverrideGroup(
        NLS::Engine::Assets::PrefabArtifact& prefab,
        const std::vector<PrefabOverrideDescriptor>& overrides,
        bool targetGeneratedReadOnly = false) const;
    PrefabOperationResult ApplyPrefabInstance(
        NLS::Engine::Assets::PrefabArtifact& prefab,
        const std::vector<PrefabOverrideDescriptor>& overrides,
        bool targetGeneratedReadOnly = false) const;
    PrefabApplyTarget ResolveNearestEditableApplyTarget(
        NLS::Engine::Assets::PrefabArtifact& candidatePrefab,
        const PrefabOverrideDescriptor& overrideRecord) const;
    PrefabOperationResult RevertSingleOverride(
        PrefabInstanceRecord& instance,
        const PrefabOverrideDescriptor& overrideRecord) const;
    PrefabOperationResult RevertOverrideGroup(
        PrefabInstanceRecord& instance,
        const std::vector<PrefabOverrideDescriptor>& overrides) const;
    PrefabOperationResult RevertPrefabInstance(PrefabInstanceRecord& instance) const;
    std::optional<NLS::Engine::Serialize::ObjectId> GetCorrespondingObjectFromSource(
        const PrefabInstanceRecord& instance,
        const NLS::Engine::GameObject& instanceObject) const;
    std::optional<NLS::Engine::Serialize::ObjectId> GetOriginalSourceObject(
        const PrefabInstanceRecord& instance,
        const NLS::Engine::GameObject& instanceObject) const;
    NLS::Engine::GameObject* GetNearestPrefabInstanceRoot(
        const PrefabInstanceRecord& instance,
        const NLS::Engine::GameObject& instanceObject) const;
    NLS::Engine::GameObject* GetOutermostPrefabInstanceRoot(
        const PrefabInstanceRecord& instance,
        const NLS::Engine::GameObject& instanceObject) const;

    PrefabOperationResult ValidateNestedPrefabs(
        const std::vector<NLS::Engine::Assets::PrefabArtifact>& prefabs) const;
    MissingPrefabRecoveryRecord BuildMissingPrefabRecoveryRecord(
        const PrefabInstanceRecord& instance) const;
    MissingPrefabRecoveryRecord BuildMissingPrefabRecoveryRecord(
        const PrefabInstanceRecord& instance,
        const std::vector<PrefabOverrideDescriptor>& overrides) const;
    PrefabOperationResult UnpackPrefabInstance(
        PrefabInstanceRecord& instance,
        PrefabUnpackMode mode) const;
    void AnnotateSceneDocumentWithPrefabInstances(
        NLS::Engine::Serialize::ObjectGraphDocument& sceneDocument,
        const NLS::Engine::SceneSystem::Scene& scene,
        const PrefabInstanceRegistry& instanceRegistry) const;
    PrefabOperationResult RestorePrefabInstancesFromSceneDocument(
        const NLS::Engine::Serialize::ObjectGraphDocument& sceneDocument,
        NLS::Engine::SceneSystem::Scene& scene,
        NLS::Core::Assets::AssetId sceneAssetId,
        PrefabInstanceRegistry& instanceRegistry,
        const std::function<std::optional<NLS::Engine::Assets::PrefabArtifact>(
            NLS::Core::Assets::AssetId,
            const std::string&)>& prefabResolver) const;
    PrefabOperationResult RestorePrefabInstancesFromSceneDocument(
        const NLS::Engine::Serialize::ObjectGraphDocument& sceneDocument,
        NLS::Engine::SceneSystem::Scene& scene,
        NLS::Core::Assets::AssetId sceneAssetId,
        PrefabInstanceRegistry& instanceRegistry,
        const std::function<std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact>(
            NLS::Core::Assets::AssetId,
            const std::string&)>& prefabResolver) const;
};
}
