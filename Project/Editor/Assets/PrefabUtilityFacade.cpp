#include "Assets/PrefabUtilityFacade.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "Assets/EditorAssetPath.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Components/TransformComponent.h"
#include "Serialize/ObjectGraphWriter.h"

namespace NLS::Editor::Assets
{
namespace
{
using NLS::Engine::Assets::PrefabArtifact;
using NLS::Engine::Assets::PrefabOverridePatchKind;
using NLS::Engine::Serialize::ObjectIdentifier;

std::string NormalizePrefabIdentityAssetPath(const std::string& sourceAssetPath)
{
    if (sourceAssetPath.empty() || sourceAssetPath.front() == ':')
        return {};

    auto normalized = NormalizeEditorAssetPath(sourceAssetPath);
    if (normalized == "Assets" || normalized.rfind("Assets/", 0u) == 0u)
        return normalized;
    return NormalizeEditorAssetPath(std::filesystem::path("Assets") / normalized);
}

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

void AddDiagnostic(
    PrefabEditorState& state,
    std::string code,
    std::string message)
{
    state.diagnostics.push_back({std::move(code), std::move(message)});
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

const NLS::Engine::Serialize::PropertyRecord* FindProperty(
    const NLS::Engine::Serialize::ObjectRecord& record,
    const char* name)
{
    for (const auto& property : record.properties)
    {
        if (property.name == name)
            return &property;
    }
    return nullptr;
}

NLS::Engine::Serialize::PropertyRecord* FindMutableProperty(
    NLS::Engine::Serialize::ObjectRecord& record,
    const char* name)
{
    for (auto& property : record.properties)
    {
        if (property.name == name)
            return &property;
    }
    return nullptr;
}

const NLS::Engine::Serialize::ObjectRecord* FindObjectRecord(
    const NLS::Engine::Serialize::ObjectGraphDocument& graph,
    const NLS::Engine::Serialize::ObjectId& id)
{
    for (const auto& record : graph.objects)
    {
        if (record.id == id)
            return &record;
    }
    return nullptr;
}

const NLS::Engine::Serialize::ObjectRecord* FindObjectRecordAtSceneObjectIndex(
    const NLS::Engine::Serialize::ObjectGraphDocument& graph,
    const NLS::Engine::Serialize::PropertyValue::ArrayValue& sceneObjectValues,
    const size_t sceneObjectIndex)
{
    if (sceneObjectIndex >= sceneObjectValues.size())
        return nullptr;

    const auto& value = sceneObjectValues[sceneObjectIndex];
    if (value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::OwnedReference)
        return nullptr;

    return FindObjectRecord(graph, value.GetObjectId());
}

NLS::Engine::Serialize::ObjectRecord* FindMutableObjectRecord(
    NLS::Engine::Serialize::ObjectGraphDocument& graph,
    const NLS::Engine::Serialize::ObjectId& id)
{
    for (auto& record : graph.objects)
    {
        if (record.id == id)
            return &record;
    }
    return nullptr;
}

NLS::Engine::Serialize::ObjectRecord* FindMutableObjectRecordAtSceneObjectIndex(
    NLS::Engine::Serialize::ObjectGraphDocument& graph,
    const NLS::Engine::Serialize::PropertyValue::ArrayValue& sceneObjectValues,
    const size_t sceneObjectIndex)
{
    if (sceneObjectIndex >= sceneObjectValues.size())
        return nullptr;

    const auto& value = sceneObjectValues[sceneObjectIndex];
    if (value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::OwnedReference)
        return nullptr;

    return FindMutableObjectRecord(graph, value.GetObjectId());
}

void UpsertScenePrefabProperty(
    NLS::Engine::Serialize::ObjectRecord& record,
    const NLS::Engine::Serialize::ObjectIdentifier& reference)
{
    if (auto* property = FindMutableProperty(record, "scenePrefab"))
    {
        property->value = NLS::Engine::Serialize::PropertyValue::ObjectReference(reference);
        return;
    }

    record.properties.push_back({
        "scenePrefab",
        NLS::Engine::Serialize::PropertyValue::ObjectReference(reference)
    });
}

bool ScenePrefabRecordIsGeneratedReadOnly(
    const NLS::Engine::Serialize::ObjectRecord& record)
{
    if (const auto* property = FindProperty(record, "scenePrefabGeneratedReadOnly");
        property != nullptr &&
        property->value.GetKind() == NLS::Engine::Serialize::PropertyValue::Kind::Bool)
    {
        return property->value.GetBool();
    }
    return true;
}

void UpsertScenePrefabGeneratedReadOnlyProperty(
    NLS::Engine::Serialize::ObjectRecord& record,
    const bool generatedReadOnly)
{
    if (auto* property = FindMutableProperty(record, "scenePrefabGeneratedReadOnly"))
    {
        property->value = NLS::Engine::Serialize::PropertyValue::Bool(generatedReadOnly);
        return;
    }

    record.properties.push_back({
        "scenePrefabGeneratedReadOnly",
        NLS::Engine::Serialize::PropertyValue::Bool(generatedReadOnly)
    });
}

void RegisterScenePrefabRecoveryInstance(
    PrefabInstanceRegistry& instanceRegistry,
    NLS::Engine::GameObject& sceneRoot,
    NLS::Core::Assets::AssetId sceneAssetId,
    NLS::Core::Assets::AssetId prefabAssetId,
    const std::string& prefabSubAssetKey,
    const bool generatedReadOnly,
    const NLS::Engine::Serialize::PrefabInstanceRecord* preservedPrefabInstance = nullptr)
{
    constexpr std::string_view kMissingSuffix = " (missing)";
    if (!sceneRoot.GetName().ends_with(kMissingSuffix))
        sceneRoot.SetName(sceneRoot.GetName() + std::string(kMissingSuffix));

    std::function<void(NLS::Engine::GameObject&)> suppressStaleRendering =
        [&](NLS::Engine::GameObject& object)
    {
        if (auto* meshFilter = object.GetComponent<NLS::Engine::Components::MeshFilter>())
            meshFilter->SetMeshReference({});
        if (auto* meshRenderer = object.GetComponent<NLS::Engine::Components::MeshRenderer>())
        {
            meshRenderer->RemoveAllMaterials();
            meshRenderer->SetTransientRenderingSuppressed(true);
        }

        for (auto* child : object.GetChildren())
        {
            if (child)
                suppressStaleRendering(*child);
        }
    };
    suppressStaleRendering(sceneRoot);

    PrefabInstanceRecord recoveryInstance;
    recoveryInstance.prefabAssetId = prefabAssetId;
    recoveryInstance.sceneAssetId = sceneAssetId;
    recoveryInstance.prefabSubAssetKey = prefabSubAssetKey;
    recoveryInstance.generatedReadOnly = generatedReadOnly;
    recoveryInstance.instanceRoot = &sceneRoot;
    if (preservedPrefabInstance)
    {
        recoveryInstance.localPatches = preservedPrefabInstance->modifications;
        recoveryInstance.preservedInstanceRootObject = preservedPrefabInstance->instanceRoot;
        recoveryInstance.preservedAddedObjects = preservedPrefabInstance->addedObjects;
        recoveryInstance.preservedCorrespondence = preservedPrefabInstance->correspondence;
    }
    instanceRegistry.Register(std::move(recoveryInstance));
    instanceRegistry.MarkAssetMissing(prefabAssetId, prefabSubAssetKey, true);
}

void RegisterScenePrefabRecoveryInstance(
    PrefabInstanceRegistry& instanceRegistry,
    NLS::Engine::GameObject& sceneRoot,
    NLS::Core::Assets::AssetId sceneAssetId,
    NLS::Core::Assets::AssetId prefabAssetId,
    const std::string& prefabSubAssetKey,
    const NLS::Engine::Serialize::ObjectRecord& rootRecord)
{
    RegisterScenePrefabRecoveryInstance(
        instanceRegistry,
        sceneRoot,
        sceneAssetId,
        prefabAssetId,
        prefabSubAssetKey,
        ScenePrefabRecordIsGeneratedReadOnly(rootRecord));
}

std::unordered_map<const NLS::Engine::GameObject*, NLS::Engine::Serialize::ObjectId> BuildSceneObjectIdsByObject(
    const NLS::Engine::Serialize::ObjectGraphDocument& sceneDocument,
    const NLS::Engine::SceneSystem::Scene& scene,
    const NLS::Engine::Serialize::PropertyValue::ArrayValue& rootValues)
{
    std::unordered_map<const NLS::Engine::GameObject*, NLS::Engine::Serialize::ObjectId> idsByObject;
    const auto& sceneObjects = scene.GetGameObjects();
    size_t sceneObjectIndex = 0u;
    for (size_t rootIndex = 0u; rootIndex < rootValues.size(); ++rootIndex)
    {
        if (rootValues[rootIndex].GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::OwnedReference)
            continue;

        const auto id = rootValues[rootIndex].GetObjectId();
        const auto* record = FindObjectRecord(sceneDocument, id);
        if (!record || !NLS::Engine::Serialize::IsInstantiableRecordState(record->state))
            continue;

        if (sceneObjectIndex >= sceneObjects.size())
            break;

        auto* sceneObject = sceneObjects[sceneObjectIndex++];
        if (!sceneObject)
            continue;

        idsByObject.emplace(sceneObject, id);
    }
    return idsByObject;
}

std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::GameObject*> BuildSceneObjectsByObjectId(
    const NLS::Engine::Serialize::ObjectGraphDocument& sceneDocument,
    NLS::Engine::SceneSystem::Scene& scene,
    const NLS::Engine::Serialize::PropertyValue::ArrayValue& rootValues)
{
    std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::GameObject*> objectsById;
    const auto& sceneObjects = scene.GetGameObjects();
    size_t sceneObjectIndex = 0u;
    for (size_t rootIndex = 0u; rootIndex < rootValues.size(); ++rootIndex)
    {
        if (rootValues[rootIndex].GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::OwnedReference)
            continue;

        const auto id = rootValues[rootIndex].GetObjectId();
        const auto* record = FindObjectRecord(sceneDocument, id);
        if (!record || !NLS::Engine::Serialize::IsInstantiableRecordState(record->state))
            continue;

        if (sceneObjectIndex >= sceneObjects.size())
            break;

        auto* sceneObject = sceneObjects[sceneObjectIndex++];
        if (!sceneObject)
            continue;

        objectsById.emplace(id, sceneObject);
    }
    return objectsById;
}

std::vector<NLS::Engine::Serialize::PrefabInstanceObjectCorrespondence> BuildPrefabInstanceCorrespondence(
    const PrefabInstanceRecord& instance,
    const std::unordered_map<const NLS::Engine::GameObject*, NLS::Engine::Serialize::ObjectId>& sceneObjectIdsByObject)
{
    std::vector<NLS::Engine::Serialize::PrefabInstanceObjectCorrespondence> correspondence;
    correspondence.reserve(instance.sourceByInstanceObject.size());
    for (const auto& mapping : instance.sourceByInstanceObject)
    {
        const auto sceneObject = sceneObjectIdsByObject.find(mapping.first);
        if (sceneObject == sceneObjectIdsByObject.end())
            continue;

        correspondence.push_back({mapping.second, sceneObject->second});
    }

    std::sort(
        correspondence.begin(),
        correspondence.end(),
        [](const auto& lhs, const auto& rhs)
        {
            return lhs.sourceObject < rhs.sourceObject;
        });
    return correspondence;
}

std::optional<std::string> ReadStringProperty(
    const NLS::Engine::Serialize::ObjectRecord& record,
    const char* propertyName)
{
    const auto* property = FindProperty(record, propertyName);
    if (!property || property->value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::String)
        return std::nullopt;

    return property->value.GetString();
}

void CollectOwnedObjectIds(
    const NLS::Engine::Serialize::PropertyValue& value,
    std::vector<NLS::Engine::Serialize::ObjectId>& ownedObjects)
{
    switch (value.GetKind())
    {
    case NLS::Engine::Serialize::PropertyValue::Kind::OwnedReference:
        ownedObjects.push_back(value.GetObjectId());
        break;
    case NLS::Engine::Serialize::PropertyValue::Kind::Array:
        for (const auto& item : value.GetArray())
            CollectOwnedObjectIds(item, ownedObjects);
        break;
    case NLS::Engine::Serialize::PropertyValue::Kind::Object:
        for (const auto& property : value.GetObject())
            CollectOwnedObjectIds(property.second, ownedObjects);
        break;
    default:
        break;
    }
}

std::vector<NLS::Engine::Serialize::ObjectId> ReadOwnedReferences(
    const NLS::Engine::Serialize::ObjectRecord& record,
    const char* propertyName)
{
    const auto* property = FindProperty(record, propertyName);
    if (!property || property->value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::Array)
        return {};

    std::vector<NLS::Engine::Serialize::ObjectId> references;
    for (const auto& value : property->value.GetArray())
    {
        if (value.GetKind() == NLS::Engine::Serialize::PropertyValue::Kind::OwnedReference)
            references.push_back(value.GetObjectId());
    }
    return references;
}

nlohmann::json ConvertPropertyValueToJson(
    const NLS::Engine::Serialize::PropertyValue& value)
{
    using NLS::Engine::Serialize::PropertyValue;

    switch (value.GetKind())
    {
    case PropertyValue::Kind::Null:
        return nullptr;
    case PropertyValue::Kind::Bool:
        return value.GetBool();
    case PropertyValue::Kind::Integer:
        return value.GetInteger();
    case PropertyValue::Kind::Number:
        return value.GetNumber();
    case PropertyValue::Kind::String:
        return value.GetString();
    case PropertyValue::Kind::Guid:
        return value.GetGuid().ToString();
    case PropertyValue::Kind::Array:
    {
        nlohmann::json output = nlohmann::json::array();
        for (const auto& item : value.GetArray())
            output.push_back(ConvertPropertyValueToJson(item));
        return output;
    }
    case PropertyValue::Kind::Object:
    {
        nlohmann::json output = nlohmann::json::object();
        for (const auto& property : value.GetObject())
            output[property.first] = ConvertPropertyValueToJson(property.second);
        return output;
    }
    default:
        return nullptr;
    }
}

bool IsTransformProperty(const std::string& propertyName)
{
    return propertyName == "localPosition" ||
        propertyName == "localRotation" ||
        propertyName == "localScale";
}

void ApplyTransformProperty(
    NLS::Engine::Components::TransformComponent& transform,
    const std::string& propertyName,
    const NLS::Engine::Serialize::PropertyValue& value)
{
    if (!IsTransformProperty(propertyName))
        return;

    const auto field = transform.GetType().GetField(propertyName);
    if (!field.IsValid() || field.IsReadOnly())
        return;

    auto instance = NLS::meta::Variant(&transform, NLS::meta::variant_policy::WrapObject {});
    auto fieldValue = field.GetType().DeserializeJson(NLS::Json(ConvertPropertyValueToJson(value)));
    field.SetValue(instance, fieldValue);
}

std::optional<NLS::Engine::Serialize::ObjectId> ApplySceneTransformRecord(
    const NLS::Engine::Serialize::ObjectGraphDocument& sceneDocument,
    const NLS::Engine::Serialize::ObjectRecord& rootRecord,
    NLS::Engine::Components::TransformComponent& transform)
{
    static const std::string transformTypeName =
        NLS_TYPEOF(NLS::Engine::Components::TransformComponent).GetName();

    for (const auto& componentId : ReadOwnedReferences(rootRecord, "components"))
    {
        const auto* componentRecord = FindObjectRecord(sceneDocument, componentId);
        if (!componentRecord || componentRecord->typeName != transformTypeName)
            continue;

        for (const auto& property : componentRecord->properties)
            ApplyTransformProperty(transform, property.name, property.value);
        return componentRecord->id;
    }
    return std::nullopt;
}

void ApplyMissingPrefabTransformModifications(
    const NLS::Engine::Serialize::PrefabInstanceRecord& prefabInstance,
    const NLS::Engine::Serialize::ObjectId& rootTransformRecordId,
    NLS::Engine::Components::TransformComponent& transform)
{
    for (const auto& modification : prefabInstance.modifications)
    {
        if (modification.type != NLS::Engine::Serialize::PatchOperationType::ReplaceProperty)
            continue;
        if (modification.target != rootTransformRecordId)
            continue;

        ApplyTransformProperty(transform, modification.property, modification.value);
    }
}

std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::Serialize::ObjectId> BuildSourceToSceneObjectMap(
    const NLS::Engine::Serialize::PrefabInstanceRecord& prefabInstance)
{
    std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::Serialize::ObjectId> sceneBySource;
    sceneBySource.emplace(prefabInstance.instanceRoot, prefabInstance.instanceRoot);
    for (const auto& mapping : prefabInstance.correspondence)
        sceneBySource.emplace(mapping.sourceObject, mapping.instanceObject);
    return sceneBySource;
}

std::optional<NLS::Engine::Serialize::ObjectId> FindMatchingSceneAddedObject(
    const NLS::Engine::Serialize::ObjectGraphDocument& sceneDocument,
    const std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::Serialize::ObjectRecord*>& recordsById,
    const NLS::Engine::Serialize::PrefabInstanceRecord& prefabInstance,
    const NLS::Engine::Serialize::PatchOperation& operation)
{
    if (operation.type != NLS::Engine::Serialize::PatchOperationType::InsertOwned)
        return std::nullopt;

    const auto sourceToScene = BuildSourceToSceneObjectMap(prefabInstance);
    const auto sceneOwner = sourceToScene.find(operation.target);
    if (sceneOwner == sourceToScene.end())
        return std::nullopt;

    const auto sceneOwnerRecord = recordsById.find(sceneOwner->second);
    if (sceneOwnerRecord == recordsById.end() || !sceneOwnerRecord->second)
        return std::nullopt;

    const auto addedObject = std::find_if(
        prefabInstance.addedObjects.begin(),
        prefabInstance.addedObjects.end(),
        [&operation](const auto& record)
        {
            return record.id == operation.object;
        });
    if (addedObject == prefabInstance.addedObjects.end())
        return std::nullopt;

    std::vector<NLS::Engine::Serialize::ObjectId> candidates;
    if (operation.property == "children")
    {
        for (const auto& sceneObject : sceneDocument.objects)
        {
            const auto* parentProperty = FindProperty(sceneObject, "parent");
            if (!parentProperty ||
                parentProperty->value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference)
            {
                continue;
            }

            const auto parentId = sceneDocument.ResolveObjectReference(parentProperty->value.GetObjectReference());
            if (parentId.has_value() && *parentId == sceneOwner->second)
                candidates.push_back(sceneObject.id);
        }
    }
    else
    {
        candidates = ReadOwnedReferences(*sceneOwnerRecord->second, operation.property.c_str());
    }

    const auto expectedName = ReadStringProperty(*addedObject, "name").value_or(addedObject->debugName);
    for (const auto& candidateId : candidates)
    {
        const auto candidate = recordsById.find(candidateId);
        if (candidate == recordsById.end() || !candidate->second)
            continue;

        if (candidate->second->typeName != addedObject->typeName)
            continue;

        const auto candidateName = ReadStringProperty(*candidate->second, "name").value_or(candidate->second->debugName);
        if (candidateName == expectedName)
            return candidateId;
    }

    if (operation.hasIndex && operation.index < candidates.size())
        return candidates[operation.index];
    return std::nullopt;
}

void MarkOwnedPlaceholderSubtreeStripped(
    const NLS::Engine::Serialize::ObjectGraphDocument& sceneDocument,
    const std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::Serialize::ObjectRecord*>& recordsById,
    const NLS::Engine::Serialize::ObjectId& objectId,
    std::unordered_set<NLS::Engine::Serialize::ObjectId>& visitedObjects)
{
    if (!visitedObjects.insert(objectId).second)
        return;

    const auto foundRecord = recordsById.find(objectId);
    if (foundRecord == recordsById.end() || !foundRecord->second)
        return;

    auto* record = foundRecord->second;
    record->state = NLS::Engine::Serialize::ObjectRecordState::Stripped;

    std::vector<NLS::Engine::Serialize::ObjectId> ownedObjects;
    for (const auto& property : record->properties)
        CollectOwnedObjectIds(property.value, ownedObjects);

    for (const auto& ownedObject : ownedObjects)
        MarkOwnedPlaceholderSubtreeStripped(sceneDocument, recordsById, ownedObject, visitedObjects);

    for (const auto& candidate : recordsById)
    {
        if (!candidate.second)
            continue;

        const auto* parentProperty = FindProperty(*candidate.second, "parent");
        if (!parentProperty ||
            parentProperty->value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference)
        {
            continue;
        }

        const auto parentId = sceneDocument.ResolveObjectReference(parentProperty->value.GetObjectReference());
        if (parentId.has_value() && *parentId == objectId)
            MarkOwnedPlaceholderSubtreeStripped(sceneDocument, recordsById, candidate.first, visitedObjects);
    }
}

void MarkPrefabInstancePlaceholdersStripped(
    NLS::Engine::Serialize::ObjectGraphDocument& sceneDocument,
    const NLS::Engine::Serialize::PrefabInstanceRecord& prefabInstance)
{
    std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::Serialize::ObjectRecord*> recordsById;
    recordsById.reserve(sceneDocument.objects.size());
    for (auto& record : sceneDocument.objects)
        recordsById.emplace(record.id, &record);

    std::unordered_set<NLS::Engine::Serialize::ObjectId> strippedObjects;
    strippedObjects.insert(prefabInstance.instanceRoot);
    for (const auto& mapping : prefabInstance.correspondence)
        strippedObjects.insert(mapping.instanceObject);
    for (const auto& addedObject : prefabInstance.addedObjects)
        strippedObjects.insert(addedObject.id);
    for (const auto& operation : prefabInstance.modifications)
    {
        const auto sceneAddedObject = FindMatchingSceneAddedObject(
            sceneDocument,
            recordsById,
            prefabInstance,
            operation);
        if (sceneAddedObject.has_value())
            strippedObjects.insert(*sceneAddedObject);
    }

    std::unordered_set<NLS::Engine::Serialize::ObjectId> visitedObjects;
    visitedObjects.reserve(strippedObjects.size());
    for (const auto& objectId : strippedObjects)
        MarkOwnedPlaceholderSubtreeStripped(sceneDocument, recordsById, objectId, visitedObjects);
}

std::vector<NLS::Engine::Serialize::PatchOperation> BuildPrefabInstanceModifications(
    const PrefabInstanceRecord& instance,
    std::vector<NLS::Engine::Serialize::ObjectRecord>& addedObjects)
{
    PrefabArtifact prefab;
    prefab.assetId = instance.prefabAssetId;
    prefab.graph = instance.SourceGraph();
    prefab.generatedModelPrefab = instance.generatedReadOnly;

    auto overrides = PrefabEditorWorkflow().DiscoverOverrides(prefab, instance);
    std::vector<NLS::Engine::Serialize::PatchOperation> modifications;
    modifications.reserve(overrides.size());
    std::unordered_set<NLS::Engine::Serialize::ObjectId> addedObjectIds;
    for (auto& overrideRecord : overrides)
    {
        modifications.push_back(std::move(overrideRecord.patch));
        for (auto& objectRecord : overrideRecord.objectRecords)
        {
            if (addedObjectIds.insert(objectRecord.id).second)
                addedObjects.push_back(std::move(objectRecord));
        }
    }
    return modifications;
}

void UpsertObjectRecord(
    NLS::Engine::Serialize::ObjectGraphDocument& graph,
    const NLS::Engine::Serialize::ObjectRecord& record)
{
    auto* existing = FindMutableObjectRecord(graph, record.id);
    if (existing)
    {
        *existing = record;
        return;
    }

    graph.objects.push_back(record);
}

std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::Serialize::ObjectRecord*> BuildMutableObjectRecordIndex(
    NLS::Engine::Serialize::ObjectGraphDocument& graph)
{
    std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::Serialize::ObjectRecord*> recordsById;
    recordsById.reserve(graph.objects.size());
    for (auto& record : graph.objects)
        recordsById.emplace(record.id, &record);
    return recordsById;
}

void MarkObjectRecordRemovedRecursive(
    const std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::Serialize::ObjectRecord*>& recordsById,
    const NLS::Engine::Serialize::ObjectId& objectId,
    std::unordered_set<NLS::Engine::Serialize::ObjectId>& visitedObjects)
{
    if (!visitedObjects.insert(objectId).second)
        return;

    const auto foundRecord = recordsById.find(objectId);
    if (foundRecord == recordsById.end() || !foundRecord->second)
        return;

    auto* record = foundRecord->second;
    std::vector<NLS::Engine::Serialize::ObjectId> ownedObjects;
    for (const auto& property : record->properties)
        CollectOwnedObjectIds(property.value, ownedObjects);

    record->state = NLS::Engine::Serialize::ObjectRecordState::Removed;
    for (const auto& ownedObject : ownedObjects)
        MarkObjectRecordRemovedRecursive(recordsById, ownedObject, visitedObjects);
}

void ApplyPrefabInstanceModificationForInstantiation(
    const std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::Serialize::ObjectRecord*>& recordsById,
    const NLS::Engine::Serialize::PatchOperation& operation)
{
    const auto foundRecord = recordsById.find(operation.target);
    if (foundRecord == recordsById.end() || !foundRecord->second)
        return;

    auto* record = foundRecord->second;
    if (operation.type == NLS::Engine::Serialize::PatchOperationType::ReplaceProperty)
    {
        if (auto* property = FindMutableProperty(*record, operation.property.c_str()))
            property->value = operation.value;
        else
            record->properties.push_back({operation.property, operation.value});
        return;
    }

    if (operation.type == NLS::Engine::Serialize::PatchOperationType::RemoveObject)
    {
        std::unordered_set<NLS::Engine::Serialize::ObjectId> visitedObjects;
        MarkObjectRecordRemovedRecursive(recordsById, operation.target, visitedObjects);
        return;
    }

    auto* property = FindMutableProperty(*record, operation.property.c_str());
    if (!property || property->value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::Array)
        return;

    auto values = property->value.GetArray();
    const auto isTargetObject = [&operation](const auto& value)
    {
        return value.GetKind() == NLS::Engine::Serialize::PropertyValue::Kind::OwnedReference &&
            value.GetObjectId() == operation.object;
    };

    switch (operation.type)
    {
    case NLS::Engine::Serialize::PatchOperationType::InsertOwned:
    {
        values.erase(std::remove_if(values.begin(), values.end(), isTargetObject), values.end());
        const auto index = operation.hasIndex && operation.index < values.size()
            ? operation.index
            : values.size();
        values.insert(
            values.begin() + static_cast<std::ptrdiff_t>(index),
            NLS::Engine::Serialize::PropertyValue::OwnedReference(operation.object));
        break;
    }
    case NLS::Engine::Serialize::PatchOperationType::RemoveOwned:
        values.erase(std::remove_if(values.begin(), values.end(), isTargetObject), values.end());
        break;
    case NLS::Engine::Serialize::PatchOperationType::MoveOwned:
    {
        const auto found = std::find_if(values.begin(), values.end(), isTargetObject);
        if (found == values.end())
            return;

        auto moved = *found;
        values.erase(found);
        const auto index = operation.hasIndex && operation.index < values.size()
            ? operation.index
            : values.size();
        values.insert(values.begin() + static_cast<std::ptrdiff_t>(index), std::move(moved));
        break;
    }
    default:
        return;
    }

    property->value = NLS::Engine::Serialize::PropertyValue::Array(std::move(values));
}

void MaterializePrefabInstanceModificationsForInstantiation(
    NLS::Engine::Serialize::ObjectGraphDocument& graph,
    const NLS::Engine::Serialize::PrefabInstanceRecord& prefabInstance)
{
    for (const auto& addedObject : prefabInstance.addedObjects)
        UpsertObjectRecord(graph, addedObject);
    auto recordsById = BuildMutableObjectRecordIndex(graph);
    for (const auto& modification : prefabInstance.modifications)
        ApplyPrefabInstanceModificationForInstantiation(recordsById, modification);
    graph.overrides.clear();
}

NLS::Engine::Serialize::PropertyValue RemapLocalObjectReferenceFileId(
    const NLS::Engine::Serialize::PropertyValue& value,
    const std::unordered_map<int64_t, int64_t>& fileIdRemap)
{
    using NLS::Engine::Serialize::ObjectIdentifier;
    using NLS::Engine::Serialize::PropertyValue;

    switch (value.GetKind())
    {
    case PropertyValue::Kind::ObjectReference:
    {
        auto reference = value.GetObjectReference();
        if (reference.IsLocalObject())
        {
            if (const auto found = fileIdRemap.find(reference.localIdentifierInFile);
                found != fileIdRemap.end())
            {
                reference = ObjectIdentifier::LocalObject(found->second);
            }
        }
        return PropertyValue::ObjectReference(std::move(reference));
    }
    case PropertyValue::Kind::Array:
    {
        PropertyValue::ArrayValue remapped;
        remapped.reserve(value.GetArray().size());
        for (const auto& item : value.GetArray())
            remapped.push_back(RemapLocalObjectReferenceFileId(item, fileIdRemap));
        return PropertyValue::Array(std::move(remapped));
    }
    case PropertyValue::Kind::Object:
    {
        PropertyValue::ObjectValue remapped;
        remapped.reserve(value.GetObject().size());
        for (const auto& property : value.GetObject())
        {
            remapped.emplace_back(
                property.first,
                RemapLocalObjectReferenceFileId(property.second, fileIdRemap));
        }
        return PropertyValue::Object(std::move(remapped));
    }
    default:
        return value;
    }
}

void RemapPreservedAddedObjectLocalReferenceFileId(
    std::vector<NLS::Engine::Serialize::ObjectRecord>& addedObjects,
    const std::unordered_map<int64_t, int64_t>& fileIdRemap)
{
    if (fileIdRemap.empty())
        return;

    for (auto& addedObject : addedObjects)
    {
        for (auto& property : addedObject.properties)
            property.value = RemapLocalObjectReferenceFileId(property.value, fileIdRemap);
    }
}

void RemoveSceneLocalAddedObjectMappings(
    PrefabInstanceRecord& instance,
    const NLS::Engine::Serialize::PrefabInstanceRecord& prefabInstance)
{
    std::unordered_set<NLS::Engine::Serialize::ObjectId> addedObjectIds;
    for (const auto& addedObject : prefabInstance.addedObjects)
        addedObjectIds.insert(addedObject.id);

    for (const auto& addedObjectId : addedObjectIds)
        instance.sourceToInstance.erase(addedObjectId);

    for (auto it = instance.sourceByInstanceObject.begin(); it != instance.sourceByInstanceObject.end();)
    {
        if (addedObjectIds.find(it->second) != addedObjectIds.end())
            it = instance.sourceByInstanceObject.erase(it);
        else
            ++it;
    }
}

void AddUnityStylePrefabInstanceRecord(
    NLS::Engine::Serialize::ObjectGraphDocument& sceneDocument,
    const PrefabInstanceRecord& instance,
    const std::unordered_map<const NLS::Engine::GameObject*, NLS::Engine::Serialize::ObjectId>& sceneObjectIdsByObject)
{
    if (!instance.instanceRoot || !instance.prefabAssetId.IsValid())
        return;

    const auto rootId = sceneObjectIdsByObject.find(instance.instanceRoot);
    if (rootId == sceneObjectIdsByObject.end())
        return;

    NLS::Engine::Serialize::PrefabInstanceRecord record;
    record.instanceRoot = rootId->second;
    record.sourcePrefab = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(instance.prefabAssetId.GetGuid()),
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(
            instance.prefabAssetId.GetGuid(),
        instance.prefabSubAssetKey),
        instance.prefabSubAssetKey);
    record.generatedReadOnly = instance.generatedReadOnly;
    if (!instance.preservedAddedObjects.empty() || !instance.preservedCorrespondence.empty())
    {
        std::unordered_set<NLS::Engine::Serialize::ObjectId> sceneObjectIds;
        sceneObjectIds.reserve(sceneDocument.objects.size());
        for (const auto& object : sceneDocument.objects)
            sceneObjectIds.insert(object.id);

        record.modifications = instance.localPatches;
        record.addedObjects = instance.preservedAddedObjects;
        std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::Serialize::ObjectId> currentSceneObjectBySource;
        currentSceneObjectBySource.reserve(instance.sourceByInstanceObject.size());
        for (const auto& sourceMapping : instance.sourceByInstanceObject)
        {
            const auto sceneObject = sceneObjectIdsByObject.find(sourceMapping.first);
            if (sceneObject != sceneObjectIdsByObject.end())
                currentSceneObjectBySource.emplace(sourceMapping.second, sceneObject->second);
        }

        if (const auto* currentRootRecord = FindObjectRecord(sceneDocument, record.instanceRoot);
            instance.preservedInstanceRootObject.IsValid() && currentRootRecord != nullptr)
        {
            std::unordered_map<int64_t, int64_t> fileIdRemap;
            const auto preservedRootFileId =
                NLS::Engine::Serialize::MakeLocalIdentifierInFile(instance.preservedInstanceRootObject);
            if (preservedRootFileId != 0 &&
                currentRootRecord->localIdentifierInFile != 0 &&
                preservedRootFileId != currentRootRecord->localIdentifierInFile)
            {
                fileIdRemap.emplace(preservedRootFileId, currentRootRecord->localIdentifierInFile);
            }

            for (const auto& correspondence : instance.preservedCorrespondence)
            {
                const auto currentObject = currentSceneObjectBySource.find(correspondence.sourceObject);
                if (currentObject == currentSceneObjectBySource.end())
                    continue;

                const auto* currentRecord = FindObjectRecord(sceneDocument, currentObject->second);
                if (currentRecord == nullptr || currentRecord->localIdentifierInFile == 0)
                    continue;

                const auto preservedFileId =
                    NLS::Engine::Serialize::MakeLocalIdentifierInFile(correspondence.instanceObject);
                if (preservedFileId != 0 && preservedFileId != currentRecord->localIdentifierInFile)
                    fileIdRemap.emplace(preservedFileId, currentRecord->localIdentifierInFile);
            }

            RemapPreservedAddedObjectLocalReferenceFileId(record.addedObjects, fileIdRemap);
        }
        record.correspondence.reserve(instance.preservedCorrespondence.size());
        for (auto correspondence : instance.preservedCorrespondence)
        {
            if (const auto currentObject = currentSceneObjectBySource.find(correspondence.sourceObject);
                currentObject != currentSceneObjectBySource.end())
            {
                correspondence.instanceObject = currentObject->second;
            }
            else if (correspondence.instanceObject == instance.preservedInstanceRootObject)
            {
                correspondence.instanceObject = record.instanceRoot;
            }

            if (sceneObjectIds.find(correspondence.instanceObject) == sceneObjectIds.end())
                continue;

            record.correspondence.push_back(std::move(correspondence));
        }
    }
    else
    {
        record.modifications = BuildPrefabInstanceModifications(instance, record.addedObjects);
        record.correspondence = BuildPrefabInstanceCorrespondence(instance, sceneObjectIdsByObject);
    }

    MarkPrefabInstancePlaceholdersStripped(sceneDocument, record);
    sceneDocument.prefabInstances.push_back(std::move(record));
}

NLS::Engine::GameObject* ResolveStrippedPlaceholderParent(
    const NLS::Engine::Serialize::ObjectGraphDocument& sceneDocument,
    const NLS::Engine::Serialize::PrefabInstanceRecord& prefabInstance,
    const std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::GameObject*>& sceneObjectsById)
{
    const auto* rootRecord = FindObjectRecord(sceneDocument, prefabInstance.instanceRoot);
    if (!rootRecord)
        return nullptr;

    const auto* parentProperty = FindProperty(*rootRecord, "parent");
    if (!parentProperty || parentProperty->value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference)
        return nullptr;

    const auto parentId = sceneDocument.ResolveObjectReference(parentProperty->value.GetObjectReference());
    if (!parentId.has_value())
        return nullptr;

    const auto parent = sceneObjectsById.find(*parentId);
    return parent != sceneObjectsById.end() ? parent->second : nullptr;
}

std::vector<NLS::Engine::Serialize::ObjectId> ReadSceneDocumentObjectOrder(
    const NLS::Engine::Serialize::ObjectGraphDocument& sceneDocument)
{
    const auto* sceneRecord = FindObjectRecord(sceneDocument, sceneDocument.root);
    if (!sceneRecord)
        return {};

    return ReadOwnedReferences(*sceneRecord, "gameObjects");
}

std::optional<NLS::Engine::Serialize::ObjectId> ResolveRecordParentId(
    const NLS::Engine::Serialize::ObjectGraphDocument& sceneDocument,
    const NLS::Engine::Serialize::ObjectRecord& record)
{
    const auto* parentProperty = FindProperty(record, "parent");
    if (!parentProperty || parentProperty->value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference)
        return std::nullopt;

    return sceneDocument.ResolveObjectReference(parentProperty->value.GetObjectReference());
}

void ReorderLiveObjectsBySceneDocumentOrder(
    const NLS::Engine::Serialize::ObjectGraphDocument& sceneDocument,
    NLS::Engine::SceneSystem::Scene& scene,
    const std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::GameObject*>& liveObjectsBySceneId)
{
    const auto order = ReadSceneDocumentObjectOrder(sceneDocument);
    std::unordered_map<NLS::Engine::GameObject*, size_t> orderByObject;
    orderByObject.reserve(liveObjectsBySceneId.size());

    for (size_t index = 0u; index < order.size(); ++index)
    {
        const auto live = liveObjectsBySceneId.find(order[index]);
        if (live != liveObjectsBySceneId.end() && live->second)
            orderByObject.emplace(live->second, index);
    }

    const auto unknownOrder = static_cast<size_t>(-1);
    const auto compareBySceneOrder = [&](NLS::Engine::GameObject* lhs, NLS::Engine::GameObject* rhs)
    {
        const auto lhsOrder = orderByObject.find(lhs);
        const auto rhsOrder = orderByObject.find(rhs);
        const auto lhsIndex = lhsOrder != orderByObject.end() ? lhsOrder->second : unknownOrder;
        const auto rhsIndex = rhsOrder != orderByObject.end() ? rhsOrder->second : unknownOrder;
        return lhsIndex < rhsIndex;
    };

    auto& sceneObjects = scene.GetGameObjects();
    std::stable_sort(sceneObjects.begin(), sceneObjects.end(), compareBySceneOrder);

    std::unordered_map<NLS::Engine::GameObject*, std::unordered_map<NLS::Engine::GameObject*, size_t>> childOrderByParent;
    for (size_t index = 0u; index < order.size(); ++index)
    {
        const auto* record = FindObjectRecord(sceneDocument, order[index]);
        if (!record)
            continue;

        const auto parentId = ResolveRecordParentId(sceneDocument, *record);
        if (!parentId.has_value())
            continue;

        const auto parent = liveObjectsBySceneId.find(*parentId);
        const auto child = liveObjectsBySceneId.find(order[index]);
        if (parent == liveObjectsBySceneId.end() ||
            child == liveObjectsBySceneId.end() ||
            !parent->second ||
            !child->second)
        {
            continue;
        }

        childOrderByParent[parent->second].emplace(child->second, index);
    }

    for (auto& parentOrder : childOrderByParent)
    {
        auto& children = parentOrder.first->GetChildren();
        std::stable_sort(
            children.begin(),
            children.end(),
            [&](NLS::Engine::GameObject* lhs, NLS::Engine::GameObject* rhs)
            {
                const auto lhsOrder = parentOrder.second.find(lhs);
                const auto rhsOrder = parentOrder.second.find(rhs);
                const auto lhsIndex = lhsOrder != parentOrder.second.end() ? lhsOrder->second : unknownOrder;
                const auto rhsIndex = rhsOrder != parentOrder.second.end() ? rhsOrder->second : unknownOrder;
                return lhsIndex < rhsIndex;
            });
    }
}

std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::GameObject*> BuildRestoredLiveObjectsBySceneId(
    const NLS::Engine::Serialize::PrefabInstanceRecord& prefabInstance,
    const PrefabInstanceRecord& restoredInstance)
{
    std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::GameObject*> liveObjectsBySceneId;
    if (restoredInstance.instanceRoot)
        liveObjectsBySceneId.emplace(prefabInstance.instanceRoot, restoredInstance.instanceRoot);

    std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::Serialize::ObjectId> sceneObjectBySource;
    for (const auto& mapping : prefabInstance.correspondence)
        sceneObjectBySource.emplace(mapping.sourceObject, mapping.instanceObject);

    for (const auto& mapping : restoredInstance.sourceByInstanceObject)
    {
        const auto sceneObject = sceneObjectBySource.find(mapping.second);
        if (sceneObject != sceneObjectBySource.end())
            liveObjectsBySceneId.emplace(sceneObject->second, const_cast<NLS::Engine::GameObject*>(mapping.first));
    }

    return liveObjectsBySceneId;
}

std::string ReadStringPropertyOr(
    const NLS::Engine::Serialize::ObjectRecord& record,
    const char* propertyName,
    std::string fallback)
{
    const auto* property = FindProperty(record, propertyName);
    if (!property || property->value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::String)
        return fallback;

    return property->value.GetString();
}

bool ReadBoolPropertyOr(
    const NLS::Engine::Serialize::ObjectRecord& record,
    const char* propertyName,
    const bool fallback)
{
    const auto* property = FindProperty(record, propertyName);
    if (!property || property->value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::Bool)
        return fallback;

    return property->value.GetBool();
}

NLS::Engine::GameObject* RestoreStrippedRecoveryRoot(
    const NLS::Engine::Serialize::ObjectGraphDocument& sceneDocument,
    const NLS::Engine::Serialize::PrefabInstanceRecord& prefabInstance,
    NLS::Engine::SceneSystem::Scene& scene,
    const std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::GameObject*>& sceneObjectsById)
{
    const auto* rootRecord = FindObjectRecord(sceneDocument, prefabInstance.instanceRoot);
    if (!rootRecord)
        return nullptr;

    auto& root = scene.CreateGameObject(
        ReadStringPropertyOr(*rootRecord, "name", rootRecord->debugName.empty() ? "Missing Prefab" : rootRecord->debugName),
        ReadStringPropertyOr(*rootRecord, "tag", "Untagged"));
    root.SetActive(ReadBoolPropertyOr(*rootRecord, "active", true));
    if (auto* transform = root.GetTransform())
    {
        const auto rootTransformRecordId = ApplySceneTransformRecord(sceneDocument, *rootRecord, *transform);
        if (rootTransformRecordId.has_value())
            ApplyMissingPrefabTransformModifications(prefabInstance, *rootTransformRecordId, *transform);
    }

    if (auto* parent = ResolveStrippedPlaceholderParent(sceneDocument, prefabInstance, sceneObjectsById))
        root.SetParent(*parent);

    return &root;
}

PrefabOperationResult InstantiateStrippedPrefabInstance(
    const NLS::Engine::Serialize::ObjectGraphDocument& sceneDocument,
    const NLS::Engine::Serialize::PrefabInstanceRecord& prefabInstance,
    std::shared_ptr<const PrefabArtifact> sourcePrefab,
    NLS::Core::Assets::AssetId prefabAssetId,
    const std::string& prefabSubAssetKey,
    NLS::Core::Assets::AssetId sceneAssetId,
    NLS::Engine::SceneSystem::Scene& scene,
    const std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::GameObject*>& sceneObjectsById)
{
    if (!sourcePrefab)
        return {};

    const bool hasSceneLocalInstantiationEdits =
        !prefabInstance.modifications.empty() || !prefabInstance.addedObjects.empty();
    std::optional<PrefabArtifact> materializedPrefab;
    const PrefabArtifact* prefabForInstantiation = sourcePrefab.get();
    if (hasSceneLocalInstantiationEdits)
    {
        materializedPrefab = *sourcePrefab;
        MaterializePrefabInstanceModificationsForInstantiation(materializedPrefab->graph, prefabInstance);
        prefabForInstantiation = &*materializedPrefab;
    }

    InstantiatePrefabRequest request;
    request.constPrefab = prefabForInstantiation;
    request.prefabAssetId = prefabAssetId;
    request.prefabSubAssetKey = prefabSubAssetKey;
    request.sceneAssetId = sceneAssetId;
    request.deferAssetReferenceResolution = false;
    request.synchronousAssetReferencePrewarm = true;

    auto instantiate = ConvertResult(PrefabEditorWorkflow().InstantiatePrefab({
        request.prefab,
        request.prefabAssetId,
        request.prefabSubAssetKey,
        request.sceneAssetId,
        request.deferAssetReferenceResolution,
        request.constPrefab,
        request.synchronousAssetReferencePrewarm
    }, scene));

    if (instantiate.status != PrefabOperationStatus::Committed || !instantiate.instance.has_value())
        return instantiate;

    instantiate.instance->UseSharedSourcePrefab(sourcePrefab);
    instantiate.instance->generatedReadOnly = prefabInstance.generatedReadOnly || sourcePrefab->generatedModelPrefab;
    instantiate.instance->localPatches = prefabInstance.modifications;
    instantiate.instance->preservedAssetReferences = NLS::Engine::Assets::CollectPrefabAssetReferences(sourcePrefab->graph);
    instantiate.instance->preservedResolvedAssets = sourcePrefab->resolvedAssets;
    RemoveSceneLocalAddedObjectMappings(*instantiate.instance, prefabInstance);

    if (auto* parent = ResolveStrippedPlaceholderParent(sceneDocument, prefabInstance, sceneObjectsById);
        parent != nullptr && instantiate.instance->instanceRoot != nullptr)
    {
        instantiate.instance->instanceRoot->SetParent(*parent);
    }

    return instantiate;
}

PrefabOperationResult RestoreUnityStylePrefabInstancesFromSceneDocument(
    const NLS::Engine::Serialize::ObjectGraphDocument& sceneDocument,
    NLS::Engine::SceneSystem::Scene& scene,
    NLS::Core::Assets::AssetId sceneAssetId,
    PrefabInstanceRegistry& instanceRegistry,
    const std::function<std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact>(
        NLS::Core::Assets::AssetId,
        const std::string&)>& prefabResolver,
    const NLS::Engine::Serialize::PropertyValue::ArrayValue& rootValues)
{
    PrefabOperationResult result;
    result.status = PrefabOperationStatus::Committed;

    auto sceneObjectsById = BuildSceneObjectsByObjectId(sceneDocument, scene, rootValues);
    bool restoredLiveObjectOrderDirty = false;
    for (const auto& prefabInstance : sceneDocument.prefabInstances)
    {
        const auto sceneRoot = sceneObjectsById.find(prefabInstance.instanceRoot);
        auto* liveSceneRoot = sceneRoot != sceneObjectsById.end() ? sceneRoot->second : nullptr;

        if (!prefabInstance.sourcePrefab.guid.IsValid())
        {
            result.status = PrefabOperationStatus::Failed;
            AddDiagnostic(
                result,
                "prefab-instance-source-invalid",
                "Scene prefab instance could not be restored because its source prefab reference was invalid.");
            continue;
        }

        const auto prefabAssetId = NLS::Core::Assets::AssetId(prefabInstance.sourcePrefab.guid);
        const auto prefabSubAssetKey = prefabInstance.sourcePrefab.filePath;
        auto prefab = prefabResolver ? prefabResolver(prefabAssetId, prefabSubAssetKey) : nullptr;
        if (!prefab)
        {
            if (!liveSceneRoot)
            {
                liveSceneRoot = RestoreStrippedRecoveryRoot(
                    sceneDocument,
                    prefabInstance,
                    scene,
                    sceneObjectsById);
            }

            if (liveSceneRoot)
            {
                sceneObjectsById.emplace(prefabInstance.instanceRoot, liveSceneRoot);
                restoredLiveObjectOrderDirty = true;
                RegisterScenePrefabRecoveryInstance(
                    instanceRegistry,
                    *liveSceneRoot,
                    sceneAssetId,
                    prefabAssetId,
                    prefabSubAssetKey,
                    prefabInstance.generatedReadOnly,
                    &prefabInstance);
            }
            result.status = PrefabOperationStatus::Failed;
            AddDiagnostic(
                result,
                "prefab-instance-restore-missing-artifact",
                "Scene prefab instance could not be restored because the prefab artifact was not available.");
            continue;
        }

        if (!liveSceneRoot)
        {
            auto instantiate = InstantiateStrippedPrefabInstance(
                sceneDocument,
                prefabInstance,
                prefab,
                prefabAssetId,
                prefabSubAssetKey,
                sceneAssetId,
                scene,
                sceneObjectsById);
            result.diagnostics.insert(result.diagnostics.end(), instantiate.diagnostics.begin(), instantiate.diagnostics.end());

            if (instantiate.status != PrefabOperationStatus::Committed || !instantiate.instance.has_value())
            {
                liveSceneRoot = RestoreStrippedRecoveryRoot(
                    sceneDocument,
                    prefabInstance,
                    scene,
                    sceneObjectsById);
                if (liveSceneRoot)
                {
                    sceneObjectsById.emplace(prefabInstance.instanceRoot, liveSceneRoot);
                    restoredLiveObjectOrderDirty = true;
                    RegisterScenePrefabRecoveryInstance(
                        instanceRegistry,
                        *liveSceneRoot,
                        sceneAssetId,
                        prefabAssetId,
                        prefabSubAssetKey,
                        prefabInstance.generatedReadOnly,
                        &prefabInstance);
                }

                result.status = PrefabOperationStatus::Failed;
                AddDiagnostic(
                    result,
                    "prefab-instance-restore-instantiate-failed",
                    "Scene prefab instance could not be restored from its stripped prefab instance record.");
                continue;
            }

            auto restoredLiveObjects = BuildRestoredLiveObjectsBySceneId(prefabInstance, *instantiate.instance);
            sceneObjectsById.insert(restoredLiveObjects.begin(), restoredLiveObjects.end());
            restoredLiveObjectOrderDirty = true;
            instanceRegistry.Register(static_cast<PrefabInstanceRecord&&>(*instantiate.instance));
            continue;
        }

        auto instantiationPrefab = *prefab;
        MaterializePrefabInstanceModificationsForInstantiation(instantiationPrefab.graph, prefabInstance);

        InstantiatePrefabRequest request;
        request.constPrefab = &instantiationPrefab;
        request.prefabAssetId = prefabAssetId;
        request.prefabSubAssetKey = prefabSubAssetKey;
        request.sceneAssetId = sceneAssetId;

        auto connect = ConvertResult(
            PrefabEditorWorkflow().ConnectExistingPrefabInstance(request, *liveSceneRoot));
        result.diagnostics.insert(result.diagnostics.end(), connect.diagnostics.begin(), connect.diagnostics.end());

        if (connect.status != PrefabOperationStatus::Committed || !connect.instance.has_value())
        {
            RegisterScenePrefabRecoveryInstance(
                instanceRegistry,
                *liveSceneRoot,
                sceneAssetId,
                prefabAssetId,
                prefabSubAssetKey,
                prefabInstance.generatedReadOnly,
                &prefabInstance);
            result.status = PrefabOperationStatus::Failed;
            AddDiagnostic(
                result,
                "prefab-instance-restore-connect-failed",
                "Scene prefab instance metadata was preserved because the prefab artifact could not be reconnected to the scene root.");
            continue;
        }

        connect.instance->UseSharedSourcePrefab(prefab);
        connect.instance->generatedReadOnly = prefabInstance.generatedReadOnly || prefab->generatedModelPrefab;
        connect.instance->localPatches = prefabInstance.modifications;
        connect.instance->preservedAssetReferences = NLS::Engine::Assets::CollectPrefabAssetReferences(prefab->graph);
        connect.instance->preservedResolvedAssets = prefab->resolvedAssets;
        connect.instance->preservedInstanceRootObject = prefabInstance.instanceRoot;
        connect.instance->preservedAddedObjects = prefabInstance.addedObjects;
        connect.instance->preservedCorrespondence = prefabInstance.correspondence;
        instanceRegistry.Register(static_cast<PrefabInstanceRecord&&>(*connect.instance));
    }

    if (restoredLiveObjectOrderDirty)
    {
        ReorderLiveObjectsBySceneDocumentOrder(sceneDocument, scene, sceneObjectsById);
        scene.RebuildRuntimeCachesAfterLoad();
    }

    return result;
}
}

PrefabSourceIdentity NormalizePrefabSourceIdentity(
    const std::filesystem::path& projectRoot,
    const std::string& sourceAssetPath,
    const std::string& prefabSubAssetKey,
    NLS::Core::Assets::AssetId sourceAssetId,
    NLS::Core::Assets::AssetType assetType)
{
    PrefabSourceIdentity identity;
    identity.projectRootId = projectRoot.lexically_normal().generic_string();
    identity.sourceAssetPath = NormalizePrefabIdentityAssetPath(sourceAssetPath);
    identity.prefabSubAssetKey = prefabSubAssetKey;

    const auto absolutePath = identity.sourceAssetPath.empty()
        ? std::filesystem::path {}
        : (projectRoot / std::filesystem::path(identity.sourceAssetPath)).lexically_normal();
    auto meta = absolutePath.empty()
        ? std::optional<NLS::Core::Assets::AssetMeta> {}
        : NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(absolutePath));

    if (meta.has_value())
    {
        if (!sourceAssetId.IsValid())
            sourceAssetId = meta->id;
        if (assetType == NLS::Core::Assets::AssetType::Unknown)
            assetType = meta->assetType;
        identity.importerId = meta->importerId;
        identity.importerVersion = std::max(
            meta->importerVersion,
            NLS::Core::Assets::GetCurrentImporterVersion(meta->assetType));
    }

    if (assetType == NLS::Core::Assets::AssetType::Unknown && !absolutePath.empty())
        assetType = NLS::Core::Assets::InferAssetType(absolutePath);
    if (identity.importerId.empty())
        identity.importerId = NLS::Core::Assets::InferImporterId(assetType);
    if (identity.importerVersion == 0u)
        identity.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(assetType);

    identity.sourceAssetId = sourceAssetId;
    identity.assetType = assetType;
    return identity;
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

PrefabEditorState PrefabUtilityFacade::GetPrefabEditorState(
    const PrefabEditorStateQuery& query) const
{
    PrefabEditorState state;
    if (!query.instance)
        return state;

    state.generatedReadOnly = query.instance->generatedReadOnly ||
        (query.prefab && IsGeneratedModelPrefab(*query.prefab));
    const bool instanceUnpacked = query.instance->unpacked || query.unpacked;

    if (query.sourceCorrupt || !query.instance->instanceRoot)
    {
        state.connectionState = PrefabEditorConnectionState::Invalid;
        state.applyAvailability = PrefabEditorApplyAvailability::Unavailable;
        AddDiagnostic(
            state,
            "prefab-instance-invalid",
            "Prefab instance metadata is invalid or corrupt.");
    }
    else if (instanceUnpacked || !query.instance->prefabAssetId.IsValid())
    {
        state.connectionState = instanceUnpacked
            ? PrefabEditorConnectionState::Unpacked
            : PrefabEditorConnectionState::Disconnected;
        state.applyAvailability = PrefabEditorApplyAvailability::Unavailable;
        AddDiagnostic(
            state,
            instanceUnpacked ? "prefab-instance-unpacked" : "prefab-instance-disconnected",
            instanceUnpacked
                ? "Prefab instance has been unpacked and is no longer connected to a source prefab."
                : "Prefab instance is disconnected from its source prefab.");
    }
    else if (!query.sourceAssetExists)
    {
        state.connectionState = PrefabEditorConnectionState::MissingSource;
        state.applyAvailability = PrefabEditorApplyAvailability::Unavailable;
        state.missingSource = true;
        AddDiagnostic(
            state,
            "prefab-source-missing",
            "Prefab source asset is missing; scene-local recovery data is preserved.");
    }
    else
    {
        state.connectionState = PrefabEditorConnectionState::Connected;
        if (state.generatedReadOnly)
        {
            state.applyAvailability = PrefabEditorApplyAvailability::ReadOnlyRejected;
            AddDiagnostic(
                state,
                "prefab-generated-read-only",
                "Generated model prefab assets are read-only; apply overrides to an editable variant.");
        }
        else
        {
            state.applyAvailability = PrefabEditorApplyAvailability::Allowed;
        }
    }

    if (state.applyAvailability == PrefabEditorApplyAvailability::Allowed &&
        !query.editableSourceArtifactContext)
    {
        state.applyAvailability = PrefabEditorApplyAvailability::Unavailable;
        AddDiagnostic(
            state,
            "prefab-apply-source-context-missing",
            "Prefab Apply requires an editable source prefab artifact context.");
    }

    if (query.resourcesCancelled)
    {
        state.resourceState = PrefabEditorResourceState::Cancelled;
        AddDiagnostic(
            state,
            "prefab-resources-cancelled",
            "Prefab renderer resource loading was cancelled.");
    }
    else if (query.resourcesFailed)
    {
        state.resourceState = PrefabEditorResourceState::Failed;
        AddDiagnostic(
            state,
            "prefab-resources-failed",
            "Prefab renderer resources failed to load.");
    }
    else if (query.resourcesPending)
    {
        state.resourceState = PrefabEditorResourceState::Pending;
        state.pendingResources = true;
        AddDiagnostic(
            state,
            "prefab-resources-pending",
            "Prefab renderer resources are still loading; mesh, material, and textures will appear together when ready.");
    }

    if (query.prefab &&
        state.connectionState != PrefabEditorConnectionState::Invalid &&
        state.connectionState != PrefabEditorConnectionState::Unpacked)
    {
        state.overrideCount = GetPrefabOverrides(
            *query.prefab,
            *query.instance,
            query.includeDefaultOverrides).size();
    }
    else if (state.connectionState != PrefabEditorConnectionState::Invalid &&
        state.connectionState != PrefabEditorConnectionState::Unpacked)
    {
        state.overrideCount = query.instance->localPatches.size();
    }

    state.hasOverrides = state.overrideCount > 0u;
    state.canRevert = state.hasOverrides &&
        state.connectionState != PrefabEditorConnectionState::MissingSource &&
        state.connectionState != PrefabEditorConnectionState::Disconnected;

    if (!state.hasOverrides && state.applyAvailability == PrefabEditorApplyAvailability::Allowed)
        state.applyAvailability = PrefabEditorApplyAvailability::Unavailable;

    return state;
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
        request.deferAssetReferenceResolution,
        request.constPrefab,
        request.synchronousAssetReferencePrewarm
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

void PrefabUtilityFacade::AnnotateSceneDocumentWithPrefabInstances(
    NLS::Engine::Serialize::ObjectGraphDocument& sceneDocument,
    const NLS::Engine::SceneSystem::Scene& scene,
    const PrefabInstanceRegistry& instanceRegistry) const
{
    auto* sceneRecord = FindMutableObjectRecord(sceneDocument, sceneDocument.root);
    if (!sceneRecord)
        return;

    const auto* gameObjects = FindProperty(*sceneRecord, "gameObjects");
    if (!gameObjects || gameObjects->value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::Array)
        return;

    const auto& rootValues = gameObjects->value.GetArray();
    const auto sceneObjectIdsByObject = BuildSceneObjectIdsByObject(sceneDocument, scene, rootValues);
    std::unordered_set<const PrefabInstanceRecord*> emittedInstances;
    const auto& sceneObjects = scene.GetGameObjects();
    for (size_t sceneObjectIndex = 0u; sceneObjectIndex < sceneObjects.size(); ++sceneObjectIndex)
    {
        auto* sceneObject = sceneObjects[sceneObjectIndex];
        if (!sceneObject)
            continue;

        const auto* instance = instanceRegistry.FindInstance(*sceneObject);
        if (!instance ||
            instance->instanceRoot != sceneObject ||
            !instance->prefabAssetId.IsValid())
        {
            continue;
        }

        if (emittedInstances.insert(instance).second)
            AddUnityStylePrefabInstanceRecord(sceneDocument, *instance, sceneObjectIdsByObject);
    }
}

PrefabOperationResult PrefabUtilityFacade::RestorePrefabInstancesFromSceneDocument(
    const NLS::Engine::Serialize::ObjectGraphDocument& sceneDocument,
    NLS::Engine::SceneSystem::Scene& scene,
    NLS::Core::Assets::AssetId sceneAssetId,
    PrefabInstanceRegistry& instanceRegistry,
    const std::function<std::optional<NLS::Engine::Assets::PrefabArtifact>(
        NLS::Core::Assets::AssetId,
        const std::string&)>& prefabResolver) const
{
    std::unordered_map<std::string, std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact>> sharedResolverCache;
    return RestorePrefabInstancesFromSceneDocument(
        sceneDocument,
        scene,
        sceneAssetId,
        instanceRegistry,
        [&prefabResolver, &sharedResolverCache](
            NLS::Core::Assets::AssetId assetId,
            const std::string& subAssetKey)
            -> std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact>
        {
            if (!prefabResolver)
                return nullptr;

            const auto cacheKey = assetId.ToString() + "\n" + subAssetKey;
            const auto cached = sharedResolverCache.find(cacheKey);
            if (cached != sharedResolverCache.end())
                return cached->second;

            auto prefab = prefabResolver(assetId, subAssetKey);
            if (!prefab.has_value())
            {
                sharedResolverCache.emplace(cacheKey, nullptr);
                return nullptr;
            }

            auto shared = std::make_shared<NLS::Engine::Assets::PrefabArtifact>(std::move(*prefab));
            sharedResolverCache.emplace(cacheKey, shared);
            return shared;
        });
}

PrefabOperationResult PrefabUtilityFacade::RestorePrefabInstancesFromSceneDocument(
    const NLS::Engine::Serialize::ObjectGraphDocument& sceneDocument,
    NLS::Engine::SceneSystem::Scene& scene,
    NLS::Core::Assets::AssetId sceneAssetId,
    PrefabInstanceRegistry& instanceRegistry,
    const std::function<std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact>(
        NLS::Core::Assets::AssetId,
        const std::string&)>& prefabResolver) const
{
    PrefabOperationResult result;
    result.status = PrefabOperationStatus::Committed;

    const auto* sceneRecord = FindObjectRecord(sceneDocument, sceneDocument.root);
    if (!sceneRecord)
        return result;

    const auto* gameObjects = FindProperty(*sceneRecord, "gameObjects");
    if (!gameObjects || gameObjects->value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::Array)
        return result;

    const auto& rootValues = gameObjects->value.GetArray();
    const auto legacySceneObjectsById = BuildSceneObjectsByObjectId(sceneDocument, scene, rootValues);

    if (!sceneDocument.prefabInstances.empty())
    {
        auto unityStyleRestore = RestoreUnityStylePrefabInstancesFromSceneDocument(
            sceneDocument,
            scene,
            sceneAssetId,
            instanceRegistry,
            prefabResolver,
            rootValues);
        result.status = unityStyleRestore.status;
        result.diagnostics.insert(
            result.diagnostics.end(),
            unityStyleRestore.diagnostics.begin(),
            unityStyleRestore.diagnostics.end());
    }

    for (const auto& rootValue : rootValues)
    {
        if (rootValue.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::OwnedReference)
            continue;

        const auto* rootRecord = FindObjectRecord(sceneDocument, rootValue.GetObjectId());
        if (!rootRecord)
            continue;

        const auto liveRoot = legacySceneObjectsById.find(rootRecord->id);
        if (liveRoot == legacySceneObjectsById.end() || !liveRoot->second)
            continue;

        auto* sceneRoot = liveRoot->second;

        const auto* scenePrefab = FindProperty(*rootRecord, "scenePrefab");
        if (!scenePrefab || scenePrefab->value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference)
        {
            continue;
        }

        const auto& reference = scenePrefab->value.GetObjectReference();
        if (!reference.guid.IsValid())
            continue;

        const auto prefabAssetId = NLS::Core::Assets::AssetId(reference.guid);
        const auto prefabSubAssetKey = reference.filePath;
        auto prefab = prefabResolver ? prefabResolver(prefabAssetId, prefabSubAssetKey) : nullptr;
        if (!prefab)
        {
            RegisterScenePrefabRecoveryInstance(
                instanceRegistry,
                *sceneRoot,
                sceneAssetId,
                prefabAssetId,
                prefabSubAssetKey,
                *rootRecord);
            result.status = PrefabOperationStatus::Failed;
            AddDiagnostic(
                result,
                "scene-prefab-restore-missing-artifact",
                "Scene prefab instance could not be restored because the prefab artifact was not available.");
            continue;
        }

        InstantiatePrefabRequest request;
        request.constPrefab = prefab.get();
        request.prefabAssetId = prefabAssetId;
        request.prefabSubAssetKey = prefabSubAssetKey;
        request.sceneAssetId = sceneAssetId;

        auto connect = ConvertResult(
            PrefabEditorWorkflow().ConnectExistingPrefabInstance(request, *sceneRoot));

        result.diagnostics.insert(result.diagnostics.end(), connect.diagnostics.begin(), connect.diagnostics.end());

        if (connect.status != PrefabOperationStatus::Committed || !connect.instance.has_value())
        {
            RegisterScenePrefabRecoveryInstance(
                instanceRegistry,
                *sceneRoot,
                sceneAssetId,
                prefabAssetId,
                prefabSubAssetKey,
                *rootRecord);
            result.status = PrefabOperationStatus::Failed;
            AddDiagnostic(
                result,
                "scene-prefab-restore-connect-failed",
                "Scene prefab instance metadata was preserved because the prefab artifact could not be reconnected to the scene root.");
            continue;
        }

        instanceRegistry.Register(static_cast<PrefabInstanceRecord&&>(*connect.instance));
    }

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
