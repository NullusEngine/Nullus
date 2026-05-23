#include "Assets/PrefabUtilityFacade.h"

#include <algorithm>

#include "Serialize/ObjectGraphWriter.h"

namespace NLS::Editor::Assets
{
namespace
{
using NLS::Engine::Assets::PrefabArtifact;
using NLS::Engine::Assets::PrefabOverridePatchKind;
using NLS::Engine::Serialize::ObjectIdentifier;

PrefabOperationStatus ConvertStatus(PrefabEditorOperationStatus status)
{
    switch (status)
    {
    case PrefabEditorOperationStatus::Rejected:
        return PrefabOperationStatus::Rejected;
    case PrefabEditorOperationStatus::Failed:
        return PrefabOperationStatus::Failed;
    case PrefabEditorOperationStatus::Committed:
        return PrefabOperationStatus::Committed;
    default:
        return PrefabOperationStatus::Failed;
    }
}

PrefabOperationResult ConvertResult(PrefabEditorOperationResult result)
{
    PrefabOperationResult converted;
    converted.status = ConvertStatus(result.status);
    converted.artifact = std::move(result.artifact);
    converted.instance = std::move(result.instance);
    converted.stage = std::move(result.stage);
    converted.unpack = std::move(result.unpack);
    converted.createdPrefabAssetId = result.createdPrefabAssetId;
    converted.createdPrefabPath = std::move(result.createdPrefabPath);
    converted.prefabSourceText = std::move(result.prefabSourceText);
    converted.dependencyChanges = std::move(result.dependencyChanges);
    converted.dependencyRefreshRequests = std::move(result.dependencyRefreshRequests);

    converted.diagnostics.reserve(result.diagnostics.size());
    for (auto& diagnostic : result.diagnostics)
        converted.diagnostics.push_back({std::move(diagnostic.code), std::move(diagnostic.message)});
    return converted;
}

void AddDiagnostic(
    PrefabOperationResult& result,
    std::string code,
    std::string message)
{
    result.diagnostics.push_back({std::move(code), std::move(message)});
}

PrefabOverrideKind ConvertOverrideKind(PrefabOverridePatchKind kind)
{
    switch (kind)
    {
    case PrefabOverridePatchKind::Property:
        return PrefabOverrideKind::Property;
    case PrefabOverridePatchKind::DefaultOverride:
        return PrefabOverrideKind::DefaultOverride;
    case PrefabOverridePatchKind::AddedComponent:
        return PrefabOverrideKind::AddedComponent;
    case PrefabOverridePatchKind::RemovedComponent:
        return PrefabOverrideKind::RemovedComponent;
    case PrefabOverridePatchKind::ReorderedComponent:
        return PrefabOverrideKind::ReorderedComponent;
    case PrefabOverridePatchKind::AddedGameObject:
        return PrefabOverrideKind::AddedGameObject;
    case PrefabOverridePatchKind::RemovedGameObject:
    case PrefabOverridePatchKind::RemovedObject:
        return PrefabOverrideKind::RemovedGameObject;
    case PrefabOverridePatchKind::ReorderedGameObject:
        return PrefabOverrideKind::ReorderedGameObject;
    case PrefabOverridePatchKind::NestedPrefab:
        return PrefabOverrideKind::NestedPrefab;
    default:
        return PrefabOverrideKind::Unknown;
    }
}

PrefabOverrideKind ClassifyOverrideKind(
    const PrefabOverrideRecord& overrideRecord,
    bool includeDefaultOverrides)
{
    return ConvertOverrideKind(NLS::Engine::Assets::ClassifyPrefabOverridePatch(
        overrideRecord.patch,
        includeDefaultOverrides));
}

PrefabOverrideRecord ToEditorOverride(const PrefabOverrideDescriptor& overrideRecord)
{
    PrefabOverrideRecord editorOverride;
    editorOverride.sourceObject = overrideRecord.sourceObject;
    editorOverride.instanceObject = overrideRecord.instanceObject;
    editorOverride.propertyPath = overrideRecord.propertyPath;
    editorOverride.patch = overrideRecord.patch;
    editorOverride.objectRecord = overrideRecord.objectRecord;
    editorOverride.objectRecords = overrideRecord.objectRecords;
    editorOverride.owningPrefabLayer = overrideRecord.owningPrefabLayer;
    return editorOverride;
}

std::vector<PrefabOverrideRecord> ToEditorOverrides(
    const std::vector<PrefabOverrideDescriptor>& overrides)
{
    std::vector<PrefabOverrideRecord> converted;
    converted.reserve(overrides.size());
    for (const auto& overrideRecord : overrides)
        converted.push_back(ToEditorOverride(overrideRecord));
    return converted;
}

bool IsGeneratedModelPrefab(const PrefabArtifact& prefab)
{
    return prefab.generatedModelPrefab;
}
}

PrefabAssetType PrefabUtilityFacade::GetPrefabAssetType(
    const PrefabAssetQuery& query) const
{
    if (query.missingAsset)
        return PrefabAssetType::MissingAsset;
    if (query.corrupt)
        return PrefabAssetType::Corrupt;
    if (!query.prefab)
        return PrefabAssetType::NotAPrefab;
    if (query.prefab->Validate().HasErrors())
        return PrefabAssetType::Corrupt;
    if (query.generatedModelPrefab || IsGeneratedModelPrefab(*query.prefab))
    {
        return PrefabAssetType::Model;
    }
    if (query.prefab->graph.basePrefab.has_value() || !query.prefab->baseChain.empty())
        return PrefabAssetType::Variant;
    return PrefabAssetType::Regular;
}

PrefabInstanceStatus PrefabUtilityFacade::GetPrefabInstanceStatus(
    const PrefabInstanceQuery& query) const
{
    if (!query.instance)
        return PrefabInstanceStatus::NotAPrefab;
    if (query.corrupt || !query.instance->instanceRoot)
        return PrefabInstanceStatus::Invalid;
    if (!query.instance->prefabAssetId.IsValid())
        return PrefabInstanceStatus::Disconnected;
    if (!query.assetExists)
        return PrefabInstanceStatus::MissingAsset;
    return PrefabInstanceStatus::Connected;
}

PrefabOperationResult PrefabUtilityFacade::SaveAsPrefabAsset(
    NLS::Engine::GameObject& root,
    NLS::Core::Assets::AssetId destinationAssetId,
    std::filesystem::path destinationPath) const
{
    return ConvertResult(PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        destinationAssetId,
        std::move(destinationPath)
    }));
}

PrefabOperationResult PrefabUtilityFacade::SaveAsPrefabAssetAndConnect(
    NLS::Engine::GameObject& root,
    NLS::Core::Assets::AssetId destinationAssetId,
    std::filesystem::path destinationPath,
    NLS::Engine::SceneSystem::Scene& scene,
    NLS::Core::Assets::AssetId sceneAssetId) const
{
    auto save = SaveAsPrefabAsset(root, destinationAssetId, std::move(destinationPath));
    if (save.status != PrefabOperationStatus::Committed || !save.artifact.has_value())
        return save;

    auto connect = ConvertResult(PrefabEditorWorkflow().ConnectExistingPrefabInstance({
        &*save.artifact,
        destinationAssetId,
        "prefab:" + root.GetName(),
        sceneAssetId
    }, root));

    connect.artifact = std::move(save.artifact);
    connect.createdPrefabAssetId = save.createdPrefabAssetId;
    connect.createdPrefabPath = std::move(save.createdPrefabPath);
    connect.prefabSourceText = std::move(save.prefabSourceText);
    connect.diagnostics.insert(connect.diagnostics.end(), save.diagnostics.begin(), save.diagnostics.end());

    (void)scene;
    return connect;
}

PrefabOperationResult PrefabUtilityFacade::InstantiatePrefab(
    const PrefabInstantiateRequest& request,
    NLS::Engine::SceneSystem::Scene& scene) const
{
    return ConvertResult(PrefabEditorWorkflow().InstantiatePrefab({
        request.prefab,
        request.prefabAssetId,
        request.prefabSubAssetKey,
        request.sceneAssetId,
        request.deferAssetReferenceResolution
    }, scene));
}

PrefabOperationResult PrefabUtilityFacade::LoadPrefabContents(
    const LoadPrefabContentsRequest& request) const
{
    return ConvertResult(PrefabEditorWorkflow().OpenPrefabStage({
        request.prefab,
        request.prefabAssetId,
        request.prefabSubAssetKey,
        request.generatedReadOnly || (request.prefab && IsGeneratedModelPrefab(*request.prefab)),
        request.prefabAssetPath
    }));
}

void PrefabUtilityFacade::MarkPrefabContentsDirty(PrefabStageState& stage) const
{
    PrefabEditorWorkflow().MarkStageDirty(stage);
}

PrefabOperationResult PrefabUtilityFacade::SavePrefabContents(
    PrefabStageState& stage,
    PrefabArtifact& prefab,
    PrefabInstanceRegistry* instanceRegistry) const
{
    auto result = ConvertResult(PrefabEditorWorkflow().SavePrefabStage(stage, prefab, instanceRegistry));
    if (result.status == PrefabOperationStatus::Committed && result.artifact.has_value())
        result.prefabSourceText = NLS::Engine::Serialize::ObjectGraphWriter::Write(result.artifact->graph);
    return result;
}

PrefabOperationResult PrefabUtilityFacade::UnloadPrefabContents(
    PrefabStageState& stage,
    const bool saveBeforeUnload,
    PrefabArtifact* prefab) const
{
    if (saveBeforeUnload)
    {
        if (!prefab)
        {
            PrefabOperationResult result;
            result.status = PrefabOperationStatus::Rejected;
            AddDiagnostic(
                result,
                "prefab-unload-save-missing-artifact",
                "Saving before unload requires a prefab artifact.");
            return result;
        }

        auto save = SavePrefabContents(stage, *prefab);
        if (save.status != PrefabOperationStatus::Committed)
            return save;
    }

    stage.stageScene.reset();
    stage.stageRoot = nullptr;
    stage.loaded = false;
    stage.dirty = false;

    PrefabOperationResult result;
    result.status = PrefabOperationStatus::Committed;
    return result;
}

PrefabOperationResult PrefabUtilityFacade::CreateVariant(
    const CreateEditableVariantRequest& request) const
{
    return ConvertResult(PrefabEditorWorkflow().CreateEditableVariant(request));
}

std::vector<PrefabOverrideDescriptor> PrefabUtilityFacade::GetPrefabOverrides(
    const PrefabArtifact& prefab,
    const PrefabInstanceRecord& instance,
    const bool includeDefaultOverrides) const
{
    const auto editorOverrides = PrefabEditorWorkflow().DiscoverOverrides(prefab, instance);
    std::vector<PrefabOverrideDescriptor> overrides;
    overrides.reserve(editorOverrides.size());

    for (const auto& editorOverride : editorOverrides)
    {
        PrefabOverrideDescriptor overrideRecord;
        overrideRecord.kind = ClassifyOverrideKind(editorOverride, includeDefaultOverrides);
        if (!includeDefaultOverrides && overrideRecord.kind == PrefabOverrideKind::DefaultOverride)
            continue;

        overrideRecord.sourceObject = editorOverride.sourceObject;
        overrideRecord.instanceObject = editorOverride.instanceObject;
        overrideRecord.propertyPath = editorOverride.propertyPath;
        overrideRecord.patch = editorOverride.patch;
        overrideRecord.objectRecord = editorOverride.objectRecord;
        overrideRecord.objectRecords = editorOverride.objectRecords;
        overrideRecord.owningPrefabLayer = editorOverride.owningPrefabLayer;
        const auto propertyModification = NLS::Engine::Assets::BuildPrefabPropertyModification(
            prefab,
            editorOverride.sourceObject,
            editorOverride.instanceObject,
            editorOverride.propertyPath,
            editorOverride.patch,
            editorOverride.owningPrefabLayer,
            includeDefaultOverrides);
        overrideRecord.defaultOverride = propertyModification.defaultOverride;
        overrideRecord.canApply = instance.prefabAssetId.IsValid();
        overrideRecord.canRevert = true;
        overrideRecord.baseValue = propertyModification.baseValue;
        overrideRecord.localValue = propertyModification.localValue;

        overrides.push_back(std::move(overrideRecord));
    }
    return overrides;
}

PrefabOperationResult PrefabUtilityFacade::ApplySingleOverride(
    PrefabArtifact& prefab,
    const PrefabOverrideDescriptor& overrideRecord,
    const bool targetGeneratedReadOnly) const
{
    return ApplyOverrideGroup(prefab, {overrideRecord}, targetGeneratedReadOnly);
}

PrefabOperationResult PrefabUtilityFacade::ApplyOverrideGroup(
    PrefabArtifact& prefab,
    const std::vector<PrefabOverrideDescriptor>& overrides,
    const bool targetGeneratedReadOnly) const
{
    if (targetGeneratedReadOnly || IsGeneratedModelPrefab(prefab))
    {
        PrefabOperationResult result;
        result.status = PrefabOperationStatus::Rejected;
        AddDiagnostic(
            result,
            "prefab-generated-read-only",
            "Generated model prefab assets are read-only; apply overrides to an editable variant.");
        return result;
    }

    return ConvertResult(PrefabEditorWorkflow().ApplyAllOverrides(prefab, ToEditorOverrides(overrides)));
}

PrefabOperationResult PrefabUtilityFacade::ApplyPrefabInstance(
    PrefabArtifact& prefab,
    const std::vector<PrefabOverrideDescriptor>& overrides,
    const bool targetGeneratedReadOnly) const
{
    return ApplyOverrideGroup(prefab, overrides, targetGeneratedReadOnly);
}

PrefabApplyTarget PrefabUtilityFacade::ResolveNearestEditableApplyTarget(
    PrefabArtifact& candidatePrefab,
    const PrefabOverrideDescriptor& overrideRecord) const
{
    PrefabApplyTarget target;
    target.prefabLayer = overrideRecord.owningPrefabLayer;

    if (IsGeneratedModelPrefab(candidatePrefab))
    {
        target.rejected = true;
        target.diagnostics.push_back({
            "prefab-generated-read-only",
            "Generated model prefab assets are read-only; route apply to an editable variant layer."
        });
        return target;
    }

    target.editablePrefab = &candidatePrefab;
    if (target.prefabLayer.empty())
        target.prefabLayer = candidatePrefab.graph.basePrefab.has_value()
            ? candidatePrefab.graph.basePrefab->filePath
            : std::string {};
    return target;
}

PrefabOperationResult PrefabUtilityFacade::RevertSingleOverride(
    PrefabInstanceRecord& instance,
    const PrefabOverrideDescriptor& overrideRecord) const
{
    return ConvertResult(PrefabEditorWorkflow().RevertSelectedOverride(instance, overrideRecord.patch));
}

PrefabOperationResult PrefabUtilityFacade::RevertOverrideGroup(
    PrefabInstanceRecord& instance,
    const std::vector<PrefabOverrideDescriptor>& overrides) const
{
    PrefabOperationResult result;
    result.status = PrefabOperationStatus::Committed;
    for (const auto& overrideRecord : overrides)
    {
        auto revert = RevertSingleOverride(instance, overrideRecord);
        result.diagnostics.insert(result.diagnostics.end(), revert.diagnostics.begin(), revert.diagnostics.end());
        result.dependencyRefreshRequests.insert(
            result.dependencyRefreshRequests.end(),
            revert.dependencyRefreshRequests.begin(),
            revert.dependencyRefreshRequests.end());
        if (revert.status != PrefabOperationStatus::Committed)
            result.status = revert.status;
    }
    result.instance = instance;
    return result;
}

PrefabOperationResult PrefabUtilityFacade::RevertPrefabInstance(
    PrefabInstanceRecord& instance) const
{
    return ConvertResult(PrefabEditorWorkflow().RevertAllOverrides(instance));
}

std::optional<NLS::Engine::Serialize::ObjectId> PrefabUtilityFacade::GetCorrespondingObjectFromSource(
    const PrefabInstanceRecord& instance,
    const NLS::Engine::GameObject& instanceObject) const
{
    const auto found = instance.sourceByInstanceObject.find(&instanceObject);
    if (found == instance.sourceByInstanceObject.end())
        return std::nullopt;
    return found->second;
}

std::optional<NLS::Engine::Serialize::ObjectId> PrefabUtilityFacade::GetOriginalSourceObject(
    const PrefabInstanceRecord& instance,
    const NLS::Engine::GameObject& instanceObject) const
{
    return GetCorrespondingObjectFromSource(instance, instanceObject);
}

NLS::Engine::GameObject* PrefabUtilityFacade::GetNearestPrefabInstanceRoot(
    const PrefabInstanceRecord& instance,
    const NLS::Engine::GameObject& instanceObject) const
{
    if (!instance.instanceRoot)
        return nullptr;

    if (&instanceObject == instance.instanceRoot || instanceObject.IsDescendantOf(instance.instanceRoot))
        return instance.instanceRoot;
    return nullptr;
}

NLS::Engine::GameObject* PrefabUtilityFacade::GetOutermostPrefabInstanceRoot(
    const PrefabInstanceRecord& instance,
    const NLS::Engine::GameObject& instanceObject) const
{
    return GetNearestPrefabInstanceRoot(instance, instanceObject);
}

PrefabOperationResult PrefabUtilityFacade::ValidateNestedPrefabs(
    const std::vector<PrefabArtifact>& prefabs) const
{
    return ConvertResult(PrefabEditorWorkflow().ValidateNestedPrefabs(prefabs));
}

MissingPrefabRecoveryRecord PrefabUtilityFacade::BuildMissingPrefabRecoveryRecord(
    const PrefabInstanceRecord& instance) const
{
    return BuildMissingPrefabRecoveryRecord(instance, {});
}

MissingPrefabRecoveryRecord PrefabUtilityFacade::BuildMissingPrefabRecoveryRecord(
    const PrefabInstanceRecord& instance,
    const std::vector<PrefabOverrideDescriptor>& overrides) const
{
    MissingPrefabRecoveryRecord recovery;
    recovery.missingPrefabAssetId = instance.prefabAssetId;
    recovery.missingPrefabSubAssetKey = instance.prefabSubAssetKey;
    recovery.preservedInstanceRoot = instance.instanceRoot;
    recovery.preservedOverrides = instance.localPatches;
    recovery.preservedOverrideRecords = overrides;
    recovery.preservedSourceToInstance = instance.sourceToInstance;
    recovery.preservedSourceByInstanceObject = instance.sourceByInstanceObject;
    return recovery;
}

PrefabOperationResult PrefabUtilityFacade::UnpackPrefabInstance(
    PrefabInstanceRecord& instance,
    const PrefabUnpackMode mode) const
{
    auto nestedReferences = instance.preservedAssetReferences;
    auto nestedInstances = instance.nestedInstances;
    nestedReferences.erase(
        std::remove_if(
            nestedReferences.begin(),
            nestedReferences.end(),
            [](const ObjectIdentifier& reference)
            {
                return !reference.guid.IsValid();
            }),
        nestedReferences.end());

    auto result = ConvertResult(PrefabEditorWorkflow().UnpackPrefabInstance(instance));
    if (result.status != PrefabOperationStatus::Committed)
        return result;

    if (mode == PrefabUnpackMode::OutermostRoot)
    {
        result.preservedNestedPrefabLinks = std::move(nestedReferences);
        instance.nestedInstances = std::move(nestedInstances);
        if (result.instance.has_value())
            result.instance->nestedInstances = instance.nestedInstances;
    }
    else
    {
        result.detachedNestedPrefabLinks = std::move(nestedReferences);
    }
    return result;
}

}
