#include "Assets/PrefabEditorWorkflow.h"

#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Profiling/PerformanceStageStats.h"
#include "Reflection/Type.h"
#include "Reflection/TypeCreator.h"
#include "Serialize/ObjectGraphInstantiator.h"
#include "Serialize/ObjectReferenceResolver.h"
#include "Serialize/ObjectGraphSerializer.h"
#include "Serialize/ObjectGraphWriter.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <unordered_set>
#include <utility>

namespace NLS::Editor::Assets
{
namespace
{
using NLS::Engine::GameObject;
using NLS::Engine::Assets::PrefabArtifact;
using NLS::Engine::Assets::PrefabResolvedAsset;
using NLS::Engine::Serialize::ObjectGraphDocument;
using NLS::Engine::Serialize::ObjectId;
using NLS::Engine::Serialize::ObjectIdentifier;
using NLS::Engine::Serialize::ObjectRecord;
using NLS::Engine::Serialize::PatchOperation;
using NLS::Engine::Serialize::PatchOperationType;
using NLS::Engine::Serialize::PropertyRecord;
using NLS::Engine::Serialize::PropertyValue;

const std::string& GameObjectTypeName()
{
    static const std::string name = NLS_TYPEOF(GameObject).GetName();
    return name;
}

bool IsGameObjectRecord(const ObjectRecord& record)
{
    return record.typeName == GameObjectTypeName();
}

ObjectId MakePrefabInstanceObjectId(
    const std::string& instanceSeed,
    const ObjectId& sourceObject)
{
    return ObjectId(NLS::Guid::NewDeterministic(
        "Prefab.Instance:" + instanceSeed + ":" + sourceObject.GetGuid().ToString()));
}

std::vector<PrefabInstanceRecord> BuildNestedInstanceRecords(
    const PrefabArtifact& artifact,
    const std::vector<NLS::Engine::Serialize::ObjectIdentifier>& references,
    NLS::Core::Assets::AssetId sceneAssetId,
    GameObject* parentRoot);

void AddDiagnostic(
    PrefabEditorOperationResult& result,
    std::string code,
    std::string message)
{
    result.diagnostics.push_back({std::move(code), std::move(message)});
}

void AddDependencyRefreshRequest(
    PrefabEditorOperationResult& result,
    NLS::Core::Assets::AssetDependencyRecord dependency)
{
    result.dependencyRefreshRequests.push_back(std::move(dependency));
}

void AddDependencyChange(
    PrefabEditorOperationResult& result,
    NLS::Core::Assets::AssetDependencyChangeKind change,
    NLS::Core::Assets::AssetId owner,
    NLS::Core::Assets::AssetDependencyRecord dependency)
{
    result.dependencyChanges.push_back({change, owner, std::move(dependency)});
}

void AddSerializationDiagnostics(
    PrefabEditorOperationResult& result,
    const NLS::Engine::Serialize::SerializationDiagnosticList& diagnostics)
{
    for (const auto& diagnostic : diagnostics.GetItems())
        result.diagnostics.push_back({"prefab-serialization-diagnostic", diagnostic.GetMessage()});
}

bool HasMultipleSelectedRoots(const CreatePrefabFromSelectionRequest& request)
{
    return request.selectedRoots.size() > 1u;
}

const ObjectRecord* FindObjectRecord(
    const ObjectGraphDocument& graph,
    const ObjectId& id)
{
    for (const auto& object : graph.objects)
    {
        if (object.id == id)
            return &object;
    }
    return nullptr;
}

ObjectRecord* FindMutableObjectRecord(
    ObjectGraphDocument& graph,
    const ObjectId& id)
{
    for (auto& object : graph.objects)
    {
        if (object.id == id)
            return &object;
    }
    return nullptr;
}

std::optional<std::string> ReadStringProperty(
    const ObjectRecord* record,
    const std::string& propertyName)
{
    if (!record)
        return std::nullopt;

    for (const auto& property : record->properties)
    {
        if (property.name == propertyName &&
            property.value.GetKind() == NLS::Engine::Serialize::PropertyValue::Kind::String)
        {
            return property.value.GetString();
        }
    }

    return std::nullopt;
}

const PropertyRecord* FindProperty(const ObjectRecord& record, const std::string& propertyName)
{
    for (const auto& property : record.properties)
    {
        if (property.name == propertyName)
            return &property;
    }
    return nullptr;
}

PropertyRecord* FindMutableProperty(ObjectRecord& record, const std::string& propertyName)
{
    for (auto& property : record.properties)
    {
        if (property.name == propertyName)
            return &property;
    }
    return nullptr;
}

std::vector<ObjectId> ReadOwnedArray(
    const ObjectRecord& record,
    const std::string& propertyName)
{
    std::vector<ObjectId> ids;
    const auto* property = FindProperty(record, propertyName);
    if (!property || property->value.GetKind() != PropertyValue::Kind::Array)
        return ids;

    for (const auto& value : property->value.GetArray())
    {
        if (value.GetKind() == PropertyValue::Kind::OwnedReference)
            ids.push_back(value.GetObjectId());
    }
    return ids;
}

std::optional<PropertyValue> ReadPropertyValue(
    const ObjectRecord& record,
    const std::string& propertyName)
{
    const auto* property = FindProperty(record, propertyName);
    if (!property)
        return std::nullopt;
    return property->value;
}

bool PropertyValuesEqual(
    const PropertyValue& lhs,
    const PropertyValue& rhs)
{
    if (lhs.GetKind() != rhs.GetKind())
        return false;

    switch (lhs.GetKind())
    {
    case PropertyValue::Kind::Null:
        return true;
    case PropertyValue::Kind::Bool:
        return lhs.GetBool() == rhs.GetBool();
    case PropertyValue::Kind::Integer:
        return lhs.GetInteger() == rhs.GetInteger();
    case PropertyValue::Kind::Number:
        return lhs.GetNumber() == rhs.GetNumber();
    case PropertyValue::Kind::String:
        return lhs.GetString() == rhs.GetString();
    case PropertyValue::Kind::Guid:
        return lhs.GetGuid() == rhs.GetGuid();
    case PropertyValue::Kind::OwnedReference:
        return lhs.GetObjectId() == rhs.GetObjectId();
    case PropertyValue::Kind::ObjectReference:
        return lhs.GetObjectReference() == rhs.GetObjectReference() &&
            lhs.GetObjectReference().filePath == rhs.GetObjectReference().filePath;
    case PropertyValue::Kind::Array:
    {
        const auto& lhsArray = lhs.GetArray();
        const auto& rhsArray = rhs.GetArray();
        if (lhsArray.size() != rhsArray.size())
            return false;

        for (size_t index = 0; index < lhsArray.size(); ++index)
        {
            if (!PropertyValuesEqual(lhsArray[index], rhsArray[index]))
                return false;
        }
        return true;
    }
    case PropertyValue::Kind::Object:
    {
        const auto& lhsObject = lhs.GetObject();
        const auto& rhsObject = rhs.GetObject();
        if (lhsObject.size() != rhsObject.size())
            return false;

        for (size_t index = 0; index < lhsObject.size(); ++index)
        {
            if (lhsObject[index].first != rhsObject[index].first ||
                !PropertyValuesEqual(lhsObject[index].second, rhsObject[index].second))
            {
                return false;
            }
        }
        return true;
    }
    default:
        return false;
    }
}

bool ContainsAssetReferenceValue(const PropertyValue& value)
{
    switch (value.GetKind())
    {
    case PropertyValue::Kind::ObjectReference:
        return value.GetObjectReference().guid.IsValid();
    case PropertyValue::Kind::Array:
        return std::any_of(
            value.GetArray().begin(),
            value.GetArray().end(),
            ContainsAssetReferenceValue);
    case PropertyValue::Kind::Object:
        return std::any_of(
            value.GetObject().begin(),
            value.GetObject().end(),
            [](const auto& property)
            {
                return ContainsAssetReferenceValue(property.second);
            });
    default:
        return false;
    }
}

nlohmann::json ConvertPropertyValueToJson(const PropertyValue& value)
{
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
    case PropertyValue::Kind::ObjectReference:
    {
        const auto& reference = value.GetObjectReference();
        return reference.guid.IsValid() ? nlohmann::json(reference.filePath) : nlohmann::json(nullptr);
    }
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

void ApplyLiveComponentRecordFields(
    NLS::Engine::Components::Component& component,
    const ObjectRecord& record)
{
    auto componentInstance = NLS::meta::Variant(&component, NLS::meta::variant_policy::WrapObject {});
    const auto type = component.GetType();
    if (!type.IsValid())
        return;

    for (const auto& property : record.properties)
    {
        if (ContainsAssetReferenceValue(property.value))
            continue;

        const auto field = type.GetField(property.name);
        if (!field.IsValid() || field.IsReadOnly())
            continue;

        const auto json = ConvertPropertyValueToJson(property.value);
        auto value = field.GetType().DeserializeJson(NLS::Json(json));
        field.SetValue(componentInstance, value);
    }
}

std::optional<std::string> ReadStringPropertyValue(
    const ObjectRecord& record,
    const std::string& propertyName)
{
    const auto value = ReadPropertyValue(record, propertyName);
    if (!value.has_value() || value->GetKind() != PropertyValue::Kind::String)
        return std::nullopt;
    return value->GetString();
}

const ObjectRecord* FindComponentRecordByType(
    const ObjectGraphDocument& graph,
    const std::vector<ObjectId>& componentIds,
    const std::string& typeName)
{
    for (const auto& componentId : componentIds)
    {
        const auto* record = FindObjectRecord(graph, componentId);
        if (record && record->typeName == typeName)
            return record;
    }
    return nullptr;
}

const ObjectRecord* FindNextUnmatchedComponentRecordByType(
    const ObjectGraphDocument& graph,
    const std::vector<ObjectId>& componentIds,
    const std::string& typeName,
    const std::unordered_set<ObjectId>& matchedIds)
{
    for (const auto& componentId : componentIds)
    {
        if (matchedIds.find(componentId) != matchedIds.end())
            continue;

        const auto* record = FindObjectRecord(graph, componentId);
        if (record && record->typeName == typeName)
            return record;
    }
    return nullptr;
}

ObjectId FindSourceObjectForInstance(
    const PrefabInstanceRecord& instance,
    const GameObject* instanceObject)
{
    const auto found = instance.sourceByInstanceObject.find(instanceObject);
    if (found != instance.sourceByInstanceObject.end())
        return found->second;
    return {};
}

ObjectId GetMappedInstanceId(
    const PrefabInstanceRecord& instance,
    const ObjectId& sourceObject)
{
    const auto found = instance.sourceToInstance.find(sourceObject);
    return found != instance.sourceToInstance.end() ? found->second : ObjectId {};
}

ObjectId MakeAddedObjectId(
    const PrefabInstanceRecord& instance,
    const std::string& propertyName,
    const std::string& typeName,
    const std::string& localName,
    size_t index)
{
    return ObjectId(NLS::Guid::NewDeterministic(
        "Prefab.LocalOverride:" +
        instance.prefabAssetId.GetGuid().ToString() + ":" +
        instance.prefabSubAssetKey + ":" +
        propertyName + ":" +
        typeName + ":" +
        localName + ":" +
        std::to_string(index)));
}

PrefabOverrideRecord MakeOverrideRecord(
    const PrefabInstanceRecord& instance,
    const ObjectId& sourceObject,
    const std::string& propertyPath,
    PatchOperation patch)
{
    PrefabOverrideRecord record;
    record.sourceObject = sourceObject;
    record.instanceObject = GetMappedInstanceId(instance, sourceObject);
    record.propertyPath = propertyPath;
    record.patch = std::move(patch);
    record.owningPrefabLayer = instance.prefabSubAssetKey;
    return record;
}

bool IsSameOwnedSequence(const std::vector<ObjectId>& lhs, const std::vector<ObjectId>& rhs)
{
    return lhs == rhs;
}

void AddMovedOwnedOverrides(
    const PrefabInstanceRecord& instance,
    const ObjectId& owner,
    const std::string& propertyName,
    const std::vector<ObjectId>& sourceOrder,
    const std::vector<ObjectId>& liveSourceOrder,
    std::vector<PrefabOverrideRecord>& overrides)
{
    if (IsSameOwnedSequence(sourceOrder, liveSourceOrder))
        return;

    for (size_t index = 0; index < liveSourceOrder.size(); ++index)
    {
        const auto& sourceObject = liveSourceOrder[index];
        if (index < sourceOrder.size() && sourceOrder[index] == sourceObject)
            continue;

        overrides.push_back(MakeOverrideRecord(
            instance,
            owner,
            propertyName,
            PatchOperation::MoveOwned(owner, propertyName, sourceObject, index)));
        return;
    }
}

ObjectRecord SerializeAddedComponent(
    NLS::Engine::Components::Component& component,
    const ObjectId& objectId)
{
    auto record = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeObjectRecord(component, objectId);
    record.localIdentifierInFile = NLS::Engine::Serialize::MakeLocalIdentifierInFile(objectId);
    record.debugName = component.GetType().GetName();
    return record;
}

void DiscoverMatchedComponentPropertyOverrides(
    const PrefabInstanceRecord& instance,
    const ObjectRecord& sourceComponent,
    NLS::Engine::Components::Component& liveComponent,
    std::vector<PrefabOverrideRecord>& overrides)
{
    auto liveRecord = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeObjectRecord(
        liveComponent,
        sourceComponent.id);

    for (const auto& liveProperty : liveRecord.properties)
    {
        const auto* sourceProperty = FindProperty(sourceComponent, liveProperty.name);
        if (sourceProperty && PropertyValuesEqual(sourceProperty->value, liveProperty.value))
            continue;

        overrides.push_back(MakeOverrideRecord(
            instance,
            sourceComponent.id,
            liveProperty.name,
            PatchOperation::ReplaceProperty(
                sourceComponent.id,
                liveProperty.name,
                liveProperty.value)));
    }
}

std::vector<ObjectRecord> SerializeAddedGameObjectSubtree(GameObject& root, const ObjectId& rootId)
{
    auto prefab = NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(root);
    std::vector<ObjectRecord> records = std::move(prefab.graph.objects);
    for (auto& record : records)
    {
        if (record.id == prefab.graph.root)
        {
            const auto oldRootId = record.id;
            const auto oldRootLocalIdentifierInFile = record.localIdentifierInFile;
            record.id = rootId;
            record.localIdentifierInFile = NLS::Engine::Serialize::MakeLocalIdentifierInFile(rootId);
            for (auto& other : records)
            {
                for (auto& property : other.properties)
                {
                    if (property.value.GetKind() == PropertyValue::Kind::ObjectReference &&
                        !property.value.GetObjectReference().guid.IsValid() &&
                        property.value.GetObjectReference().localIdentifierInFile == oldRootLocalIdentifierInFile)
                    {
                        property.value = PropertyValue::ObjectReference(ObjectIdentifier::LocalObject(record.localIdentifierInFile));
                    }
                }
            }
            break;
        }
    }
    return records;
}

bool SetSerializedRootParent(
    std::vector<ObjectRecord>& records,
    const ObjectId& rootId,
    const ObjectId& parentId,
    const ObjectGraphDocument& referenceGraph)
{
    for (auto& record : records)
    {
        if (record.id != rootId)
            continue;

        const auto parentReference = referenceGraph.TryMakeObjectReference(parentId);
        if (!parentReference.has_value())
            return false;

        if (auto* parent = FindMutableProperty(record, "parent"))
            parent->value = PropertyValue::ObjectReference(*parentReference);
        else
            record.properties.push_back({
                "parent",
                PropertyValue::ObjectReference(*parentReference)
            });
        return true;
    }
    return false;
}

void RewriteObjectIdValue(
    PropertyValue& value,
    int64_t fromFileID,
    const ObjectIdentifier& toReference,
    const ObjectId& from,
    const ObjectId& to)
{
    switch (value.GetKind())
    {
    case PropertyValue::Kind::OwnedReference:
        if (value.GetObjectId() == from)
            value = PropertyValue::OwnedReference(to);
        break;
    case PropertyValue::Kind::ObjectReference:
        if (!value.GetObjectReference().guid.IsValid() &&
            value.GetObjectReference().localIdentifierInFile == fromFileID)
        {
            value = PropertyValue::ObjectReference(toReference);
        }
        break;
    case PropertyValue::Kind::Array:
    {
        auto values = value.GetArray();
        for (auto& item : values)
            RewriteObjectIdValue(item, fromFileID, toReference, from, to);
        value = PropertyValue::Array(std::move(values));
        break;
    }
    case PropertyValue::Kind::Object:
    {
        auto values = value.GetObject();
        for (auto& property : values)
            RewriteObjectIdValue(property.second, fromFileID, toReference, from, to);
        value = PropertyValue::Object(std::move(values));
        break;
    }
    default:
        break;
    }
}

void RewriteObjectId(ObjectGraphDocument& graph, const ObjectId& from, const ObjectId& to)
{
    if (!from.IsValid() || !to.IsValid() || from == to)
        return;

    const int64_t fromFileID = graph.GetLocalIdentifierInFileForObject(from);
    if (fromFileID == 0)
        return;

    const auto toReference = ObjectIdentifier::LocalObject(NLS::Engine::Serialize::MakeLocalIdentifierInFile(to));

    for (auto& record : graph.objects)
    {
        if (record.id == from)
        {
            record.id = to;
            record.localIdentifierInFile = toReference.localIdentifierInFile;
        }

        for (auto& property : record.properties)
            RewriteObjectIdValue(property.value, fromFileID, toReference, from, to);
    }

    if (graph.root == from)
        graph.root = to;

    for (auto& patch : graph.overrides)
    {
        if (patch.target == from)
            patch.target = to;
        if (patch.object == from)
            patch.object = to;
        RewriteObjectIdValue(patch.value, fromFileID, toReference, from, to);
    }
}

void CollectExistingObjectIdRewrites(
    const ObjectGraphDocument& source,
    const ObjectGraphDocument& saved,
    const ObjectId& sourceId,
    const ObjectId& savedId,
    std::vector<std::pair<ObjectId, ObjectId>>& rewrites)
{
    const auto* sourceRecord = FindObjectRecord(source, sourceId);
    const auto* savedRecord = FindObjectRecord(saved, savedId);
    if (!sourceRecord || !savedRecord)
        return;

    rewrites.push_back({savedId, sourceId});

    const auto sourceComponents = ReadOwnedArray(*sourceRecord, "components");
    const auto savedComponents = ReadOwnedArray(*savedRecord, "components");
    const auto componentCount = std::min(sourceComponents.size(), savedComponents.size());
    for (size_t index = 0; index < componentCount; ++index)
    {
        const auto* sourceComponent = FindObjectRecord(source, sourceComponents[index]);
        const auto* savedComponent = FindObjectRecord(saved, savedComponents[index]);
        if (sourceComponent && savedComponent && sourceComponent->typeName == savedComponent->typeName)
            rewrites.push_back({savedComponents[index], sourceComponents[index]});
    }

    const auto sourceChildren = ReadOwnedArray(*sourceRecord, "children");
    const auto savedChildren = ReadOwnedArray(*savedRecord, "children");
    const auto childCount = std::min(sourceChildren.size(), savedChildren.size());
    for (size_t index = 0; index < childCount; ++index)
        CollectExistingObjectIdRewrites(source, saved, sourceChildren[index], savedChildren[index], rewrites);
}

void PreserveExistingObjectIdsFromOpenedGraph(
    ObjectGraphDocument& saved,
    const ObjectGraphDocument& openedGraph)
{
    std::vector<std::pair<ObjectId, ObjectId>> rewrites;
    CollectExistingObjectIdRewrites(openedGraph, saved, openedGraph.root, saved.root, rewrites);

    for (const auto& rewrite : rewrites)
        RewriteObjectId(saved, rewrite.first, rewrite.second);
}

std::string ComponentTypeName(const NLS::Engine::Components::Component& component)
{
    return component.GetType().GetName();
}

void MapInstanceHierarchy(
    const PrefabArtifact& prefab,
    const ObjectRecord& sourceRecord,
    GameObject& instanceObject,
    PrefabInstanceRecord& instance)
{
    const auto instanceSeed = instance.instanceRoot != nullptr
        ? std::to_string(instance.instanceRoot->GetInstanceID())
        : NLS::Guid::New().ToString();
    instance.sourceByInstanceObject.emplace(&instanceObject, sourceRecord.id);
    instance.sourceToInstance.emplace(
        sourceRecord.id,
        MakePrefabInstanceObjectId(instanceSeed, sourceRecord.id));

    const auto sourceComponents = ReadOwnedArray(sourceRecord, "components");
    const auto& liveComponents = instanceObject.GetComponents();
    const auto componentCount = std::min(sourceComponents.size(), liveComponents.size());
    for (size_t index = 0; index < componentCount; ++index)
    {
        const auto* component = liveComponents[index].get();
        const auto* componentRecord = FindObjectRecord(prefab.graph, sourceComponents[index]);
        if (!component || !componentRecord)
            continue;

        instance.sourceToInstance.emplace(
            componentRecord->id,
            MakePrefabInstanceObjectId(instanceSeed, componentRecord->id));
    }

    const auto sourceChildren = ReadOwnedArray(sourceRecord, "children");
    auto& liveChildren = instanceObject.GetChildren();
    const auto childCount = std::min(sourceChildren.size(), liveChildren.size());
    for (size_t index = 0; index < childCount; ++index)
    {
        auto* child = liveChildren[index];
        const auto* childRecord = FindObjectRecord(prefab.graph, sourceChildren[index]);
        if (child && childRecord)
            MapInstanceHierarchy(prefab, *childRecord, *child, instance);
    }
}

void ApplyRecordToLiveGameObject(GameObject& gameObject, const ObjectRecord& record)
{
    if (auto name = ReadStringPropertyValue(record, "name"))
        gameObject.SetName(*name);
    if (auto tag = ReadStringPropertyValue(record, "tag"))
        gameObject.SetTag(*tag);
    if (auto active = ReadPropertyValue(record, "active");
        active.has_value() && active->GetKind() == PropertyValue::Kind::Bool)
    {
        gameObject.SetActive(active->GetBool());
    }
}

GameObject* CreateLiveGameObjectFromRecord(const ObjectRecord& record)
{
    return new GameObject(
        ReadStringPropertyValue(record, "name").value_or(record.debugName),
        ReadStringPropertyValue(record, "tag").value_or(std::string {}));
}

NLS::Engine::Components::Component* EnsureLiveComponentFromRecord(
    GameObject& owner,
    const ObjectRecord& record)
{
    const auto type = NLS::meta::Type::GetFromName(record.typeName);
    if (!type.IsValid() || !type.DerivesFrom(NLS_TYPEOF(NLS::Engine::Components::Component)))
        return nullptr;

    for (const auto& component : owner.GetComponents())
    {
        if (component && component->GetType() == type)
            return component.get();
    }

    return owner.AddComponent(type);
}

NLS::Engine::Components::Component* EnsureLiveComponentFromRecordAtIndex(
    GameObject& owner,
    const ObjectRecord& record,
    const size_t index,
    const std::unordered_set<NLS::Engine::Components::Component*>& consumedComponents)
{
    const auto type = NLS::meta::Type::GetFromName(record.typeName);
    if (!type.IsValid() || !type.DerivesFrom(NLS_TYPEOF(NLS::Engine::Components::Component)))
        return nullptr;

    if (index < owner.GetComponents().size())
    {
        auto* existing = owner.GetComponents()[index].get();
        if (existing &&
            existing->GetType() == type &&
            consumedComponents.find(existing) == consumedComponents.end())
        {
            return existing;
        }
    }

    for (const auto& component : owner.GetComponents())
    {
        if (component &&
            component->GetType() == type &&
            consumedComponents.find(component.get()) == consumedComponents.end())
        {
            owner.MoveComponent(component.get(), index);
            return component.get();
        }
    }

    return owner.AddComponent(type);
}

void ApplyDeferredAssetReferenceHintsIfMissing(
    NLS::Engine::Components::Component& component,
    const ObjectRecord& record)
{
    if (record.typeName == NLS_TYPEOF(NLS::Engine::Components::MeshFilter).GetName() &&
        component.GetType() == NLS_TYPEOF(NLS::Engine::Components::MeshFilter))
    {
        auto& meshFilter = static_cast<NLS::Engine::Components::MeshFilter&>(component);
        // SetMeshObjectIdentifier clears transient preview meshes; prefab graph tasks keep the canonical path.
        if (!meshFilter.GetModelPath().empty() || meshFilter.HasResolvedTransientMesh())
            return;

        const auto mesh = ReadPropertyValue(record, "mesh");
        if (mesh.has_value() &&
            mesh->GetKind() == PropertyValue::Kind::ObjectReference &&
            mesh->GetObjectReference().guid.IsValid())
        {
            meshFilter.SetMeshObjectIdentifier(mesh->GetObjectReference());
        }
        return;
    }

    if (record.typeName == NLS_TYPEOF(NLS::Engine::Components::MeshRenderer).GetName() &&
        component.GetType() == NLS_TYPEOF(NLS::Engine::Components::MeshRenderer))
    {
        auto& meshRenderer = static_cast<NLS::Engine::Components::MeshRenderer&>(component);
        const auto materials = ReadPropertyValue(record, "materials");
        if (!materials.has_value() || materials->GetKind() != PropertyValue::Kind::Array)
            return;

        const auto existingPaths = meshRenderer.GetMaterialPaths();
        NLS::Array<std::string> paths;
        paths.reserve(materials->GetArray().size());
        bool changed = false;
        size_t index = 0u;
        for (const auto& value : materials->GetArray())
        {
            std::string path;
            if (value.GetKind() == PropertyValue::Kind::ObjectReference &&
                value.GetObjectReference().guid.IsValid())
            {
                path = NLS::Engine::Serialize::ResolveAssetReferencePath(value.GetObjectReference());
            }

            if (path.empty() && index < existingPaths.size())
                path = existingPaths[index];
            if (path.empty())
            {
                if (auto* material = meshRenderer.GetMaterialAtIndex(static_cast<uint32_t>(index));
                    material != nullptr)
                {
                    path = material->path;
                }
            }

            if (index >= existingPaths.size() || existingPaths[index] != path)
                changed = true;
            paths.push_back(std::move(path));
            ++index;
        }

        if (changed)
            meshRenderer.SetMaterialPathHints(paths);
    }
}

GameObject* FindLiveChildForSourceRecord(
    GameObject& owner,
    const ObjectRecord& childRecord,
    const std::unordered_set<GameObject*>& consumedChildren)
{
    const auto childName = ReadStringPropertyValue(childRecord, "name").value_or(childRecord.debugName);
    const auto childTag = ReadStringPropertyValue(childRecord, "tag").value_or(std::string {});
    for (auto* child : owner.GetChildren())
    {
        if (!child)
            continue;
        if (consumedChildren.find(child) != consumedChildren.end())
            continue;
        if (child->GetName() == childName && (childTag.empty() || child->GetTag() == childTag))
            return child;
    }
    return nullptr;
}

void CompleteLiveGameObjectSubtreeFromPrefab(
    const ObjectGraphDocument& graph,
    const ObjectRecord& sourceRecord,
    GameObject& liveObject,
    PrefabInstanceRecord& instance)
{
    const auto instanceSeed = instance.instanceRoot != nullptr
        ? std::to_string(instance.instanceRoot->GetInstanceID())
        : NLS::Guid::New().ToString();
    ApplyRecordToLiveGameObject(liveObject, sourceRecord);
    instance.sourceByInstanceObject.emplace(&liveObject, sourceRecord.id);
    instance.sourceToInstance.emplace(
        sourceRecord.id,
        MakePrefabInstanceObjectId(instanceSeed, sourceRecord.id));

    const auto sourceComponents = ReadOwnedArray(sourceRecord, "components");
    std::unordered_set<NLS::Engine::Components::Component*> consumedComponents;
    for (size_t index = 0u; index < sourceComponents.size(); ++index)
    {
        const auto* componentRecord = FindObjectRecord(graph, sourceComponents[index]);
        if (!componentRecord)
            continue;

        auto* component = EnsureLiveComponentFromRecordAtIndex(
            liveObject,
            *componentRecord,
            index,
            consumedComponents);
        if (!component)
            continue;

        consumedComponents.insert(component);
        ApplyLiveComponentRecordFields(*component, *componentRecord);
        ApplyDeferredAssetReferenceHintsIfMissing(*component, *componentRecord);
        instance.sourceToInstance.emplace(
            componentRecord->id,
            MakePrefabInstanceObjectId(instanceSeed, componentRecord->id));
    }

    std::unordered_set<GameObject*> consumedChildren;
    for (const auto& childId : ReadOwnedArray(sourceRecord, "children"))
    {
        const auto* childRecord = FindObjectRecord(graph, childId);
        if (!childRecord || !IsGameObjectRecord(*childRecord))
            continue;

        auto* child = FindLiveChildForSourceRecord(liveObject, *childRecord, consumedChildren);
        if (!child)
        {
            child = CreateLiveGameObjectFromRecord(*childRecord);
            if (!child)
                continue;
            child->SetParent(liveObject);
        }

        consumedChildren.insert(child);
        CompleteLiveGameObjectSubtreeFromPrefab(graph, *childRecord, *child, instance);
    }
}

void RestoreLiveGameObjectSubtree(
    const ObjectGraphDocument& graph,
    const ObjectRecord& sourceRecord,
    GameObject& liveObject,
    PrefabInstanceRecord& instance)
{
    const auto instanceSeed = instance.instanceRoot != nullptr
        ? std::to_string(instance.instanceRoot->GetInstanceID())
        : NLS::Guid::New().ToString();
    ApplyRecordToLiveGameObject(liveObject, sourceRecord);
    instance.sourceByInstanceObject.emplace(&liveObject, sourceRecord.id);
    instance.sourceToInstance.emplace(
        sourceRecord.id,
        MakePrefabInstanceObjectId(instanceSeed, sourceRecord.id));

    for (const auto& componentId : ReadOwnedArray(sourceRecord, "components"))
    {
        const auto* componentRecord = FindObjectRecord(graph, componentId);
        if (!componentRecord)
            continue;

        auto* component = EnsureLiveComponentFromRecord(liveObject, *componentRecord);
        if (!component)
            continue;

        instance.sourceToInstance.emplace(
            componentRecord->id,
            MakePrefabInstanceObjectId(instanceSeed, componentRecord->id));
    }

    for (const auto& childId : ReadOwnedArray(sourceRecord, "children"))
    {
        const auto* childRecord = FindObjectRecord(graph, childId);
        if (!childRecord || !IsGameObjectRecord(*childRecord))
            continue;

        auto* child = CreateLiveGameObjectFromRecord(*childRecord);
        if (!child)
            continue;

        child->SetParent(liveObject);
        RestoreLiveGameObjectSubtree(graph, *childRecord, *child, instance);
    }
}

GameObject* FindLiveObjectForSource(
    const PrefabInstanceRecord& instance,
    const ObjectId& sourceObject)
{
    if (instance.SourceGraph().root == sourceObject)
        return instance.instanceRoot;

    for (const auto& mapping : instance.sourceByInstanceObject)
    {
        if (mapping.second == sourceObject)
            return const_cast<GameObject*>(mapping.first);
    }
    return nullptr;
}

NLS::Engine::Components::Component* FindLiveComponentForSource(
    const PrefabInstanceRecord& instance,
    const ObjectId& sourceComponent)
{
    const auto& sourceGraph = instance.SourceGraph();
    const auto* sourceRecord = FindObjectRecord(sourceGraph, sourceComponent);
    if (!sourceRecord)
        return nullptr;

    for (const auto& mapping : instance.sourceByInstanceObject)
    {
        auto* owner = const_cast<GameObject*>(mapping.first);
        const auto* ownerRecord = FindObjectRecord(sourceGraph, mapping.second);
        if (!owner || !ownerRecord)
            continue;

        const auto sourceComponents = ReadOwnedArray(*ownerRecord, "components");
        const auto componentIt = std::find(sourceComponents.begin(), sourceComponents.end(), sourceComponent);
        if (componentIt == sourceComponents.end())
            continue;

        const auto componentIndex = static_cast<size_t>(std::distance(sourceComponents.begin(), componentIt));
        if (componentIndex >= owner->GetComponents().size())
            return nullptr;

        auto* component = owner->GetComponents()[componentIndex].get();
        if (component && ComponentTypeName(*component) == sourceRecord->typeName)
            return component;
        return nullptr;
    }

    return nullptr;
}

std::vector<NLS::Engine::Components::Component*> FindAddedLiveComponents(
    const ObjectGraphDocument& sourceGraph,
    const ObjectRecord& sourceOwner,
    GameObject& liveOwner)
{
    std::unordered_map<std::string, size_t> remainingSourceComponentTypes;
    for (const auto& componentId : ReadOwnedArray(sourceOwner, "components"))
    {
        const auto* componentRecord = FindObjectRecord(sourceGraph, componentId);
        if (componentRecord)
            ++remainingSourceComponentTypes[componentRecord->typeName];
    }

    std::vector<NLS::Engine::Components::Component*> addedComponents;
    for (const auto& component : liveOwner.GetComponents())
    {
        auto* liveComponent = component.get();
        if (!liveComponent)
            continue;

        const auto typeName = liveComponent->GetType().GetName();
        auto found = remainingSourceComponentTypes.find(typeName);
        if (found != remainingSourceComponentTypes.end() && found->second > 0u)
        {
            --found->second;
            continue;
        }

        addedComponents.push_back(liveComponent);
    }
    return addedComponents;
}

bool RevertLiveStructuralPatch(
    PrefabInstanceRecord& instance,
    const PatchOperation& patch)
{
    const auto& sourceGraph = instance.SourceGraph();
    if (!sourceGraph.root.IsValid())
        return true;

    switch (patch.type)
    {
    case PatchOperationType::InsertOwned:
    {
        auto* owner = FindLiveObjectForSource(instance, patch.target);
        if (!owner)
            return false;

        if (patch.property == "components")
        {
            const auto* sourceOwner = FindObjectRecord(sourceGraph, patch.target);
            if (!sourceOwner)
                return false;

            auto addedComponents = FindAddedLiveComponents(sourceGraph, *sourceOwner, *owner);
            for (auto* component : addedComponents)
                owner->RemoveComponent(component);
            return true;
        }

        if (patch.property == "children")
        {
            auto* child = FindLiveObjectForSource(instance, patch.object);
            if (!child)
                return true;

            child->DetachFromParent();
            instance.sourceByInstanceObject.erase(child);
            return true;
        }
        return true;
    }
    case PatchOperationType::RemoveOwned:
    {
        auto* owner = FindLiveObjectForSource(instance, patch.target);
        const auto* record = FindObjectRecord(sourceGraph, patch.object);
        if (!owner || !record)
            return false;

        if (patch.property == "children" && IsGameObjectRecord(*record))
        {
            auto* restored = CreateLiveGameObjectFromRecord(*record);
            if (!restored)
                return false;

            restored->SetParent(*owner);
            RestoreLiveGameObjectSubtree(sourceGraph, *record, *restored, instance);
        }
        return true;
    }
    default:
        return true;
    }
}

bool RevertLivePropertyToPrefabValue(
    const PrefabArtifact& prefab,
    const PrefabInstanceRecord& instance,
    const PatchOperation& patch)
{
    if (patch.type != PatchOperationType::ReplaceProperty)
        return true;

    const auto sourceId = patch.target;
    const auto& instanceSourceGraph = instance.SourceGraph();
    const auto& sourceGraph = instanceSourceGraph.root.IsValid()
        ? instanceSourceGraph
        : prefab.graph;
    const auto* sourceRecord = FindObjectRecord(sourceGraph, sourceId);
    if (!sourceRecord)
        return false;

    auto* liveObject = instance.instanceRoot;
    if (sourceId != sourceGraph.root)
    {
        liveObject = nullptr;
        for (const auto& mapping : instance.sourceByInstanceObject)
        {
            if (mapping.second == sourceId)
            {
                liveObject = const_cast<GameObject*>(mapping.first);
                break;
            }
        }
    }

    if (!liveObject)
        return false;

    const auto baseValue = ReadPropertyValue(*sourceRecord, patch.property);
    if (!baseValue.has_value())
        return false;

    if (patch.property == "name" && baseValue->GetKind() == PropertyValue::Kind::String)
    {
        liveObject->SetName(baseValue->GetString());
        return true;
    }
    if (patch.property == "tag" && baseValue->GetKind() == PropertyValue::Kind::String)
    {
        liveObject->SetTag(baseValue->GetString());
        return true;
    }
    if (patch.property == "active" && baseValue->GetKind() == PropertyValue::Kind::Bool)
    {
        liveObject->SetActive(baseValue->GetBool());
        return true;
    }

    return true;
}

bool RebuildExistingInstanceFromPrefab(
    const PrefabArtifact& prefab,
    GameObject& root,
    PrefabInstanceRecord& instance)
{
    const auto* rootRecord = FindObjectRecord(prefab.graph, prefab.graph.root);
    if (!rootRecord || !IsGameObjectRecord(*rootRecord))
        return false;

    CompleteLiveGameObjectSubtreeFromPrefab(prefab.graph, *rootRecord, root, instance);
    return true;
}

bool ApplyLiveComponentLocalPatch(PrefabInstanceRecord& instance, const PatchOperation& patch)
{
    auto* component = FindLiveComponentForSource(instance, patch.target);
    if (!component)
        return false;

    const auto field = component->GetType().GetField(patch.property);
    if (!field.IsValid() || field.IsReadOnly())
        return false;

    auto componentInstance = NLS::meta::Variant(component, NLS::meta::variant_policy::WrapObject {});
    const auto json = ConvertPropertyValueToJson(patch.value);
    auto value = field.GetType().DeserializeJson(NLS::Json(json));
    field.SetValue(componentInstance, value);
    return true;
}

bool ApplyLiveLocalPatch(PrefabInstanceRecord& instance, const PatchOperation& patch)
{
    if (patch.type != PatchOperationType::ReplaceProperty)
        return true;

    auto* liveObject = FindLiveObjectForSource(instance, patch.target);
    if (!liveObject)
        return ApplyLiveComponentLocalPatch(instance, patch);

    if (patch.property == "name" && patch.value.GetKind() == PropertyValue::Kind::String)
    {
        liveObject->SetName(patch.value.GetString());
        return true;
    }
    if (patch.property == "tag" && patch.value.GetKind() == PropertyValue::Kind::String)
    {
        liveObject->SetTag(patch.value.GetString());
        return true;
    }
    if (patch.property == "active" && patch.value.GetKind() == PropertyValue::Kind::Bool)
    {
        liveObject->SetActive(patch.value.GetBool());
        return true;
    }

    return true;
}

bool RefreshConnectedInstanceFromPrefab(
    PrefabInstanceRecord& instance,
    const PrefabArtifact& prefab)
{
    if (!instance.instanceRoot)
        return false;

    const auto localPatches = instance.localPatches;
    instance.sourceGraph = prefab.graph;
    instance.sourceToInstance.clear();
    instance.sourceByInstanceObject.clear();
    instance.preservedAssetReferences = NLS::Engine::Assets::CollectPrefabAssetReferences(prefab.graph);
    instance.preservedResolvedAssets = prefab.resolvedAssets;
    instance.nestedInstances = BuildNestedInstanceRecords(
        prefab,
        instance.preservedAssetReferences,
        instance.sceneAssetId,
        instance.instanceRoot);

    if (!RebuildExistingInstanceFromPrefab(prefab, *instance.instanceRoot, instance))
        return false;

    instance.localPatches = localPatches;
    for (const auto& patch : instance.localPatches)
    {
        if (!ApplyLiveLocalPatch(instance, patch))
            return false;
    }

    return true;
}

void DiscoverComponentOverrides(
    const PrefabArtifact& prefab,
    const PrefabInstanceRecord& instance,
    const ObjectRecord& sourceOwner,
    GameObject& liveOwner,
    std::vector<PrefabOverrideRecord>& overrides)
{
    const auto sourceComponents = ReadOwnedArray(sourceOwner, "components");
    std::unordered_set<ObjectId> matchedSources;
    std::vector<ObjectId> liveSourceOrder;

    const auto& liveComponents = liveOwner.GetComponents();
    for (size_t index = 0; index < liveComponents.size(); ++index)
    {
        auto* component = liveComponents[index].get();
        if (!component)
            continue;

        const auto* sourceRecord = FindNextUnmatchedComponentRecordByType(
            prefab.graph,
            sourceComponents,
            ComponentTypeName(*component),
            matchedSources);
        if (sourceRecord && matchedSources.insert(sourceRecord->id).second)
        {
            liveSourceOrder.push_back(sourceRecord->id);
            DiscoverMatchedComponentPropertyOverrides(instance, *sourceRecord, *component, overrides);
            continue;
        }

        const auto objectId = MakeAddedObjectId(
            instance,
            "components",
            ComponentTypeName(*component),
            ComponentTypeName(*component),
            index);
        auto record = MakeOverrideRecord(
            instance,
            sourceOwner.id,
            "components",
            PatchOperation::InsertOwned(sourceOwner.id, "components", objectId, index));
        record.objectRecord = SerializeAddedComponent(*component, objectId);
        record.objectRecords.push_back(*record.objectRecord);
        overrides.push_back(std::move(record));
    }

    for (const auto& sourceComponent : sourceComponents)
    {
        if (matchedSources.find(sourceComponent) != matchedSources.end())
            continue;

        overrides.push_back(MakeOverrideRecord(
            instance,
            sourceOwner.id,
            "components",
            PatchOperation::RemoveOwned(sourceOwner.id, "components", sourceComponent)));
    }

    std::vector<ObjectId> matchedSourceOrder;
    for (const auto& sourceComponent : sourceComponents)
    {
        if (matchedSources.find(sourceComponent) != matchedSources.end())
            matchedSourceOrder.push_back(sourceComponent);
    }
    AddMovedOwnedOverrides(instance, sourceOwner.id, "components", matchedSourceOrder, liveSourceOrder, overrides);
}

void DiscoverChildOverrides(
    const PrefabArtifact& prefab,
    const PrefabInstanceRecord& instance,
    const ObjectRecord& sourceOwner,
    GameObject& liveOwner,
    std::vector<PrefabOverrideRecord>& overrides)
{
    const auto sourceChildren = ReadOwnedArray(sourceOwner, "children");
    std::unordered_set<ObjectId> matchedSources;
    std::vector<ObjectId> liveSourceOrder;

    const auto& liveChildren = liveOwner.GetChildren();
    for (size_t index = 0; index < liveChildren.size(); ++index)
    {
        auto* child = liveChildren[index];
        if (!child)
            continue;

        const auto sourceChild = FindSourceObjectForInstance(instance, child);
        if (sourceChild.IsValid() && matchedSources.insert(sourceChild).second)
        {
            liveSourceOrder.push_back(sourceChild);
            continue;
        }

        const auto matched = std::find_if(sourceChildren.begin(), sourceChildren.end(), [&](const ObjectId& candidate)
        {
            const auto* sourceRecord = FindObjectRecord(prefab.graph, candidate);
            return sourceRecord &&
                   matchedSources.find(candidate) == matchedSources.end() &&
                   ReadStringProperty(sourceRecord, "name").value_or(sourceRecord->debugName) == child->GetName();
        });

        if (matched != sourceChildren.end())
        {
            matchedSources.insert(*matched);
            liveSourceOrder.push_back(*matched);
            continue;
        }

        const auto objectId = MakeAddedObjectId(
            instance,
            "children",
            GameObjectTypeName(),
            child->GetName(),
            index);
        auto record = MakeOverrideRecord(
            instance,
            sourceOwner.id,
            "children",
            PatchOperation::InsertOwned(sourceOwner.id, "children", objectId, index));
        record.objectRecords = SerializeAddedGameObjectSubtree(*child, objectId);
        if (!SetSerializedRootParent(record.objectRecords, objectId, sourceOwner.id, prefab.graph))
            continue;
        for (const auto& objectRecord : record.objectRecords)
        {
            if (objectRecord.id == objectId)
            {
                record.objectRecord = objectRecord;
                break;
            }
        }
        overrides.push_back(std::move(record));
    }

    for (const auto& sourceChild : sourceChildren)
    {
        if (matchedSources.find(sourceChild) != matchedSources.end())
            continue;

        overrides.push_back(MakeOverrideRecord(
            instance,
            sourceOwner.id,
            "children",
            PatchOperation::RemoveOwned(sourceOwner.id, "children", sourceChild)));
        auto removeObject = MakeOverrideRecord(
            instance,
            sourceChild,
            "",
            PatchOperation {});
        removeObject.patch.type = PatchOperationType::RemoveObject;
        removeObject.patch.target = sourceChild;
        overrides.push_back(std::move(removeObject));
    }

    std::vector<ObjectId> matchedSourceOrder;
    for (const auto& sourceChild : sourceChildren)
    {
        if (matchedSources.find(sourceChild) != matchedSources.end())
            matchedSourceOrder.push_back(sourceChild);
    }
    AddMovedOwnedOverrides(instance, sourceOwner.id, "children", matchedSourceOrder, liveSourceOrder, overrides);
}

GameObject* FindMappedLiveChild(
    const PrefabInstanceRecord& instance,
    GameObject& liveOwner,
    const ObjectId& sourceChild)
{
    for (auto* child : liveOwner.GetChildren())
    {
        if (!child)
            continue;

        if (FindSourceObjectForInstance(instance, child) == sourceChild)
            return child;
    }
    return nullptr;
}

void DiscoverOverridesRecursive(
    const PrefabArtifact& prefab,
    const PrefabInstanceRecord& instance,
    const ObjectRecord& sourceOwner,
    GameObject& liveOwner,
    std::vector<PrefabOverrideRecord>& overrides)
{
    const auto baseName = ReadStringProperty(&sourceOwner, "name");
    if (baseName.has_value() && *baseName != liveOwner.GetName())
    {
        overrides.push_back(MakeOverrideRecord(
            instance,
            sourceOwner.id,
            "name",
            PatchOperation::ReplaceProperty(
                sourceOwner.id,
                "name",
                PropertyValue::String(liveOwner.GetName()))));
    }

    DiscoverComponentOverrides(prefab, instance, sourceOwner, liveOwner, overrides);
    DiscoverChildOverrides(prefab, instance, sourceOwner, liveOwner, overrides);

    for (const auto& sourceChild : ReadOwnedArray(sourceOwner, "children"))
    {
        const auto* childRecord = FindObjectRecord(prefab.graph, sourceChild);
        if (!childRecord || !IsGameObjectRecord(*childRecord))
            continue;

        auto* liveChild = FindMappedLiveChild(instance, liveOwner, sourceChild);
        if (!liveChild)
            continue;

        DiscoverOverridesRecursive(prefab, instance, *childRecord, *liveChild, overrides);
    }
}

bool ContainsObjectRecord(const ObjectGraphDocument& graph, const ObjectId& id)
{
    return FindObjectRecord(graph, id) != nullptr;
}

void UpsertObjectRecord(ObjectGraphDocument& graph, const ObjectRecord& record)
{
    if (auto* existing = FindMutableObjectRecord(graph, record.id))
    {
        *existing = record;
        return;
    }
    graph.objects.push_back(record);
}

bool ApplyReplaceProperty(ObjectGraphDocument& graph, const PatchOperation& patch)
{
    auto* target = FindMutableObjectRecord(graph, patch.target);
    if (!target)
        return false;

    if (auto* property = FindMutableProperty(*target, patch.property))
        property->value = patch.value;
    else
        target->properties.push_back({patch.property, patch.value});
    return true;
}

bool ApplyOwnedArrayPatch(ObjectGraphDocument& graph, const PatchOperation& patch)
{
    auto* target = FindMutableObjectRecord(graph, patch.target);
    if (!target)
        return false;

    auto* property = FindMutableProperty(*target, patch.property);
    if (!property || property->value.GetKind() != PropertyValue::Kind::Array)
        return false;

    auto values = property->value.GetArray();
    const auto isTargetObject = [&patch](const PropertyValue& value)
    {
        return value.GetKind() == PropertyValue::Kind::OwnedReference && value.GetObjectId() == patch.object;
    };

    switch (patch.type)
    {
    case PatchOperationType::InsertOwned:
    {
        if (!ContainsObjectRecord(graph, patch.object))
            return false;

        values.erase(std::remove_if(values.begin(), values.end(), isTargetObject), values.end());
        const auto index = patch.hasIndex && patch.index < values.size()
            ? patch.index
            : values.size();
        values.insert(values.begin() + static_cast<std::ptrdiff_t>(index), PropertyValue::OwnedReference(patch.object));
        break;
    }
    case PatchOperationType::RemoveOwned:
        values.erase(std::remove_if(values.begin(), values.end(), isTargetObject), values.end());
        break;
    case PatchOperationType::MoveOwned:
    {
        auto found = std::find_if(values.begin(), values.end(), isTargetObject);
        if (found == values.end())
            return false;

        auto moved = *found;
        values.erase(found);
        const auto index = patch.hasIndex && patch.index < values.size()
            ? patch.index
            : values.size();
        values.insert(values.begin() + static_cast<std::ptrdiff_t>(index), std::move(moved));
        break;
    }
    default:
        return false;
    }

    property->value = PropertyValue::Array(std::move(values));
    return true;
}

bool ApplyRemoveObject(ObjectGraphDocument& graph, const PatchOperation& patch)
{
    auto* record = FindMutableObjectRecord(graph, patch.target);
    if (!record)
        return false;

    const auto ownedChildren = [&record]()
    {
        std::vector<ObjectId> children;
        for (const auto& property : record->properties)
        {
            if (property.value.GetKind() != PropertyValue::Kind::Array)
                continue;

            for (const auto& value : property.value.GetArray())
            {
                if (value.GetKind() == PropertyValue::Kind::OwnedReference)
                    children.push_back(value.GetObjectId());
            }
        }
        return children;
    }();

    record->state = NLS::Engine::Serialize::ObjectRecordState::Removed;
    for (const auto& child : ownedChildren)
    {
        PatchOperation childPatch;
        childPatch.target = child;
        ApplyRemoveObject(graph, childPatch);
    }
    return true;
}

bool ApplyPatchOperation(ObjectGraphDocument& graph, const PatchOperation& patch)
{
    switch (patch.type)
    {
    case PatchOperationType::ReplaceProperty:
        return ApplyReplaceProperty(graph, patch);
    case PatchOperationType::InsertOwned:
    case PatchOperationType::RemoveOwned:
    case PatchOperationType::MoveOwned:
        return ApplyOwnedArrayPatch(graph, patch);
    case PatchOperationType::RemoveObject:
        return ApplyRemoveObject(graph, patch);
    default:
        return false;
    }
}

bool ApplyOverrideToArtifact(PrefabArtifact& artifact, const PrefabOverrideRecord& overrideRecord)
{
    for (const auto& objectRecord : overrideRecord.objectRecords)
        UpsertObjectRecord(artifact.graph, objectRecord);
    if (overrideRecord.objectRecord.has_value())
        UpsertObjectRecord(artifact.graph, *overrideRecord.objectRecord);

    if (!ApplyPatchOperation(artifact.graph, overrideRecord.patch))
        return false;

    artifact.graph.overrides.push_back(overrideRecord.patch);
    artifact.graph.overrides = NLS::Engine::Assets::NormalizePrefabOverridePatches(artifact.graph.overrides);
    return true;
}

bool IsSamePropertyValue(const PropertyValue& lhs, const PropertyValue& rhs)
{
    if (lhs.GetKind() != rhs.GetKind())
        return false;

    switch (lhs.GetKind())
    {
    case PropertyValue::Kind::Null:
        return true;
    case PropertyValue::Kind::Bool:
        return lhs.GetBool() == rhs.GetBool();
    case PropertyValue::Kind::Integer:
        return lhs.GetInteger() == rhs.GetInteger();
    case PropertyValue::Kind::Number:
        return lhs.GetNumber() == rhs.GetNumber();
    case PropertyValue::Kind::String:
        return lhs.GetString() == rhs.GetString();
    case PropertyValue::Kind::Guid:
        return lhs.GetGuid() == rhs.GetGuid();
    case PropertyValue::Kind::OwnedReference:
        return lhs.GetObjectId() == rhs.GetObjectId();
    case PropertyValue::Kind::ObjectReference:
        return lhs.GetObjectReference().guid == rhs.GetObjectReference().guid &&
            lhs.GetObjectReference().localIdentifierInFile == rhs.GetObjectReference().localIdentifierInFile &&
            lhs.GetObjectReference().fileType == rhs.GetObjectReference().fileType &&
            lhs.GetObjectReference().filePath == rhs.GetObjectReference().filePath;
    default:
        return false;
    }
}

bool IsSamePatch(const PatchOperation& lhs, const PatchOperation& rhs)
{
    if (lhs.type != rhs.type ||
        lhs.target != rhs.target ||
        lhs.property != rhs.property ||
        lhs.object != rhs.object ||
        lhs.hasIndex != rhs.hasIndex ||
        (lhs.hasIndex && lhs.index != rhs.index))
    {
        return false;
    }

    if (lhs.type == PatchOperationType::ReplaceProperty)
        return IsSamePropertyValue(lhs.value, rhs.value);

    return true;
}

bool IsSamePatchIdentity(const PatchOperation& lhs, const PatchOperation& rhs)
{
    return lhs.type == rhs.type &&
           lhs.target == rhs.target &&
           lhs.property == rhs.property &&
           lhs.object == rhs.object &&
           lhs.hasIndex == rhs.hasIndex &&
           (!lhs.hasIndex || lhs.index == rhs.index);
}

bool CanSurfaceStoredLocalPatch(const PatchOperation& patch)
{
    return patch.type == PatchOperationType::ReplaceProperty;
}

void AddLocalPatchOverrides(
    const PrefabInstanceRecord& instance,
    std::vector<PrefabOverrideRecord>& overrides)
{
    for (const auto& patch : instance.localPatches)
    {
        if (!CanSurfaceStoredLocalPatch(patch))
            continue;

        const auto alreadyDiscovered = std::any_of(
            overrides.begin(),
            overrides.end(),
            [&patch](const PrefabOverrideRecord& overrideRecord)
            {
                return IsSamePatchIdentity(overrideRecord.patch, patch);
            });
        if (alreadyDiscovered)
            continue;

        overrides.push_back(MakeOverrideRecord(
            instance,
            patch.target,
            patch.property,
            patch));
    }
}

void AddValidationDiagnostics(
    PrefabEditorOperationResult& result,
    const NLS::Engine::Serialize::SerializationDiagnosticList& diagnostics)
{
    for (const auto& diagnostic : diagnostics.GetItems())
        result.diagnostics.push_back({"prefab-apply-validation-failed", diagnostic.GetMessage()});
}

bool IsNestedPrefabReference(
    const PrefabArtifact& artifact,
    const NLS::Engine::Serialize::ObjectIdentifier& reference)
{
    if (!reference.guid.IsValid())
        return false;

    const NLS::Core::Assets::AssetId assetId(reference.guid);
    const auto referencePath = std::filesystem::path(reference.filePath).lexically_normal();
    return std::any_of(
        artifact.resolvedAssets.begin(),
        artifact.resolvedAssets.end(),
        [&](const auto& resolved)
        {
            if (resolved.expectedType != "Prefab" ||
                resolved.assetId != assetId)
            {
                return false;
            }

            if (reference.filePath.empty())
                return true;

            const auto resolvedPath = std::filesystem::path(resolved.artifactPath).lexically_normal();
            return resolved.subAssetKey == reference.filePath ||
                resolved.artifactPath == reference.filePath ||
                (!resolved.artifactPath.empty() && resolvedPath == referencePath);
        });
}

std::vector<PrefabInstanceRecord> BuildNestedInstanceRecords(
    const PrefabArtifact& artifact,
    const std::vector<NLS::Engine::Serialize::ObjectIdentifier>& references,
    NLS::Core::Assets::AssetId sceneAssetId,
    GameObject* parentRoot = nullptr)
{
    std::vector<PrefabInstanceRecord> nested;
    for (const auto& reference : references)
    {
        if (!IsNestedPrefabReference(artifact, reference))
            continue;

        PrefabInstanceRecord record;
        record.prefabAssetId = NLS::Core::Assets::AssetId(reference.guid);
        record.sceneAssetId = sceneAssetId;
        record.prefabSubAssetKey = reference.filePath;
        if (parentRoot)
        {
            auto* root = new GameObject(reference.filePath.empty() ? "Nested Prefab" : reference.filePath, "Prefab");
            root->SetParent(*parentRoot);
            record.instanceRoot = root;
        }
        nested.push_back(std::move(record));
    }
    return nested;
}

bool HasResolvedAssetReference(
    const PrefabArtifact& artifact,
    const NLS::Engine::Serialize::ObjectIdentifier& reference)
{
    const NLS::Core::Assets::AssetId assetId(reference.guid);
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

void AddUnresolvedAssetReferenceDiagnostics(
    PrefabEditorOperationResult& result,
    const PrefabArtifact& artifact,
    const PropertyValue& value)
{
    switch (value.GetKind())
    {
    case PropertyValue::Kind::ObjectReference:
    {
        const auto& reference = value.GetObjectReference();
        if (reference.guid.IsValid() && !HasResolvedAssetReference(artifact, reference))
        {
            AddDiagnostic(
                result,
                "prefab-unresolved-asset-reference",
                "Prefab artifact contains an unresolved asset reference.");
        }
        break;
    }
    case PropertyValue::Kind::Array:
        for (const auto& item : value.GetArray())
            AddUnresolvedAssetReferenceDiagnostics(result, artifact, item);
        break;
    case PropertyValue::Kind::Object:
        for (const auto& property : value.GetObject())
            AddUnresolvedAssetReferenceDiagnostics(result, artifact, property.second);
        break;
    default:
        break;
    }
}

void AddArtifactDiagnostics(
    PrefabEditorOperationResult& result,
    const PrefabArtifact& artifact)
{
    for (const auto& object : artifact.graph.objects)
    {
        if (!NLS::meta::Type::GetFromName(object.typeName).IsValid())
        {
            AddDiagnostic(
                result,
                "prefab-unknown-editor-record",
                "Prefab artifact contains an unknown editor-safe object record type: " + object.typeName);
        }

        for (const auto& property : object.properties)
            AddUnresolvedAssetReferenceDiagnostics(result, artifact, property.value);
    }
}

std::string MakePrefabSourceStateKey(
    const NLS::Core::Assets::AssetId& assetId,
    const std::string& prefabSubAssetKey)
{
    if (!assetId.IsValid())
        return {};

    return assetId.GetGuid().ToString() + "|" + prefabSubAssetKey;
}
}

PrefabInstanceRecord& PrefabInstanceRegistry::Register(PrefabInstanceRecord instance)
{
    if (instance.instanceRoot)
    {
        for (auto& existing : m_instances)
        {
            if (existing.instanceRoot == instance.instanceRoot)
            {
                m_failedResourceInstanceRoots.erase(existing.instanceRoot);
                m_pendingResourceInstanceRoots.erase(existing.instanceRoot);
                existing = std::move(instance);
                return existing;
            }
        }
    }

    m_instances.push_back(std::move(instance));
    return m_instances.back();
}

void PrefabInstanceRegistry::Clear()
{
    m_instances.clear();
    m_missingPrefabSources.clear();
    m_pendingResourcePrefabSources.clear();
    m_failedResourceInstanceRoots.clear();
    m_pendingResourceInstanceRoots.clear();
}

PrefabInstanceRecord* PrefabInstanceRegistry::FindRootInstance(const GameObject& object)
{
    return const_cast<PrefabInstanceRecord*>(
        static_cast<const PrefabInstanceRegistry&>(*this).FindRootInstance(object));
}

const PrefabInstanceRecord* PrefabInstanceRegistry::FindRootInstance(const GameObject& object) const
{
    for (const auto& instance : m_instances)
    {
        if (instance.instanceRoot == &object)
            return &instance;
    }

    return nullptr;
}

bool PrefabInstanceRegistry::RemoveRootInstance(const GameObject& object)
{
    for (auto it = m_instances.begin(); it != m_instances.end(); ++it)
    {
        if (it->instanceRoot == &object)
        {
            m_failedResourceInstanceRoots.erase(it->instanceRoot);
            m_pendingResourceInstanceRoots.erase(it->instanceRoot);
            m_instances.erase(it);
            return true;
        }
    }

    return false;
}

bool PrefabInstanceRegistry::RemoveObjectMapping(const GameObject& object)
{
    bool removed = false;
    for (auto& instance : m_instances)
    {
        const auto erased = instance.sourceByInstanceObject.erase(&object);
        removed = removed || erased > 0u;
    }

    return removed;
}

void PrefabInstanceRegistry::MarkAssetMissing(
    NLS::Core::Assets::AssetId assetId,
    const bool missing)
{
    if (!assetId.IsValid())
        return;

    bool markedKnownInstance = false;
    for (const auto& instance : m_instances)
    {
        if (instance.prefabAssetId != assetId)
            continue;

        MarkAssetMissing(assetId, instance.prefabSubAssetKey, missing);
        markedKnownInstance = true;
    }

    if (markedKnownInstance)
        return;

    MarkAssetMissing(assetId, std::string {}, missing);
}

void PrefabInstanceRegistry::MarkAssetMissing(
    NLS::Core::Assets::AssetId assetId,
    const std::string& prefabSubAssetKey,
    const bool missing)
{
    const auto key = MakePrefabSourceStateKey(assetId, prefabSubAssetKey);
    if (key.empty())
        return;

    if (missing)
        m_missingPrefabSources.insert(key);
    else
        m_missingPrefabSources.erase(key);
}

bool PrefabInstanceRegistry::IsAssetMissing(
    NLS::Core::Assets::AssetId assetId,
    const std::string& prefabSubAssetKey) const
{
    const auto key = MakePrefabSourceStateKey(assetId, prefabSubAssetKey);
    return !key.empty() && m_missingPrefabSources.find(key) != m_missingPrefabSources.end();
}

void PrefabInstanceRegistry::MarkAssetPendingResources(
    NLS::Core::Assets::AssetId assetId,
    const bool pending)
{
    if (!assetId.IsValid())
        return;

    bool markedKnownInstance = false;
    for (const auto& instance : m_instances)
    {
        if (instance.prefabAssetId != assetId)
            continue;

        MarkAssetPendingResources(assetId, instance.prefabSubAssetKey, pending);
        markedKnownInstance = true;
    }

    if (markedKnownInstance)
        return;

    MarkAssetPendingResources(assetId, std::string {}, pending);
}

void PrefabInstanceRegistry::MarkAssetPendingResources(
    NLS::Core::Assets::AssetId assetId,
    const std::string& prefabSubAssetKey,
    const bool pending)
{
    const auto key = MakePrefabSourceStateKey(assetId, prefabSubAssetKey);
    if (key.empty())
        return;

    if (pending)
        m_pendingResourcePrefabSources.insert(key);
    else
        m_pendingResourcePrefabSources.erase(key);
}

void PrefabInstanceRegistry::MarkInstanceResourceFailure(
    const GameObject& instanceRoot,
    const bool failed)
{
    const auto* instance = FindRootInstance(instanceRoot);
    if (!instance || instance->instanceRoot == nullptr)
        return;

    if (failed)
        m_failedResourceInstanceRoots.insert(instance->instanceRoot);
    else
        m_failedResourceInstanceRoots.erase(instance->instanceRoot);
}

void PrefabInstanceRegistry::MarkInstancePendingResources(
    const GameObject& instanceRoot,
    const bool pending)
{
    const auto* instance = FindRootInstance(instanceRoot);
    if (!instance || instance->instanceRoot == nullptr)
        return;

    if (pending)
        m_pendingResourceInstanceRoots.insert(instance->instanceRoot);
    else
        m_pendingResourceInstanceRoots.erase(instance->instanceRoot);
}

void PrefabInstanceRegistry::ClearInstancePendingResourcesForPrefab(
    NLS::Core::Assets::AssetId assetId)
{
    if (!assetId.IsValid())
        return;

    for (const auto& instance : m_instances)
    {
        if (instance.prefabAssetId == assetId && instance.instanceRoot != nullptr)
            m_pendingResourceInstanceRoots.erase(instance.instanceRoot);
    }
}

PrefabInstanceRecord* PrefabInstanceRegistry::FindInstance(const GameObject& object)
{
    return const_cast<PrefabInstanceRecord*>(
        static_cast<const PrefabInstanceRegistry&>(*this).FindInstance(object));
}

const PrefabInstanceRecord* PrefabInstanceRegistry::FindInstance(const GameObject& object) const
{
    for (const auto& instance : m_instances)
    {
        if (instance.instanceRoot == &object)
            return &instance;

        if (instance.sourceByInstanceObject.find(&object) != instance.sourceByInstanceObject.end())
            return &instance;
    }

    return nullptr;
}

std::vector<PrefabInstanceRecord*> PrefabInstanceRegistry::FindInstancesForPrefab(
    NLS::Core::Assets::AssetId prefabAssetId,
    const std::string& prefabSubAssetKey)
{
    std::vector<PrefabInstanceRecord*> result;
    for (auto& instance : m_instances)
    {
        if (instance.prefabAssetId == prefabAssetId &&
            (prefabSubAssetKey.empty() || instance.prefabSubAssetKey == prefabSubAssetKey))
        {
            result.push_back(&instance);
        }
    }

    return result;
}

PrefabHierarchyPresentation PrefabInstanceRegistry::GetPresentation(const GameObject& object) const
{
    PrefabHierarchyPresentation presentation;
    const auto* instance = FindInstance(object);
    if (!instance)
        return presentation;

    presentation.assetId = instance->prefabAssetId;
    presentation.subAssetKey = instance->prefabSubAssetKey;
    presentation.generatedReadOnly = instance->generatedReadOnly;
    const auto prefabSourceStateKey =
        MakePrefabSourceStateKey(instance->prefabAssetId, instance->prefabSubAssetKey);
    presentation.missingAsset =
        m_missingPrefabSources.find(prefabSourceStateKey) != m_missingPrefabSources.end();
    presentation.pendingResources =
        m_pendingResourcePrefabSources.find(prefabSourceStateKey) != m_pendingResourcePrefabSources.end() ||
        (instance->instanceRoot != nullptr &&
            (m_pendingResourceInstanceRoots.find(instance->instanceRoot) != m_pendingResourceInstanceRoots.end() ||
                m_failedResourceInstanceRoots.find(instance->instanceRoot) != m_failedResourceInstanceRoots.end()));
    presentation.unpacked = instance->unpacked;
    presentation.hasOverrides = !instance->localPatches.empty();
    presentation.state = instance->instanceRoot == &object
        ? PrefabHierarchyState::Root
        : PrefabHierarchyState::Child;

    if (presentation.unpacked)
        presentation.color = PrefabHierarchyColorToken::Unpacked;
    else if (presentation.missingAsset)
        presentation.color = PrefabHierarchyColorToken::Missing;
    else
    {
        presentation.color = presentation.state == PrefabHierarchyState::Root
            ? PrefabHierarchyColorToken::ConnectedRoot
            : PrefabHierarchyColorToken::ConnectedChild;
    }

    return presentation;
}

const NLS::Engine::Serialize::ObjectGraphDocument& PrefabInstanceRecord::SourceGraph() const
{
    if (sharedSourcePrefab && sharedSourcePrefab->graph.root.IsValid())
        return sharedSourcePrefab->graph;
    return sourceGraph;
}

const NLS::Engine::Assets::PrefabArtifact* PrefabInstanceRecord::SharedSourcePrefab() const
{
    return sharedSourcePrefab.get();
}

void PrefabInstanceRecord::UseSharedSourcePrefab(
    std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact> prefab)
{
    sharedSourcePrefab = std::move(prefab);
    if (sharedSourcePrefab)
    {
        prefabAssetId = sharedSourcePrefab->assetId;
        generatedReadOnly = sharedSourcePrefab->generatedModelPrefab;
        sourceGraph = {};
    }
}

PrefabEditorOperationResult PrefabEditorWorkflow::CreatePrefabFromSelection(
    const CreatePrefabFromSelectionRequest& request) const
{
    PrefabEditorOperationResult result;
    result.status = PrefabEditorOperationStatus::Rejected;

    if (!request.selectedRoot)
    {
        AddDiagnostic(result, "prefab-missing-selection-root", "Prefab creation requires one selected root GameObject.");
        return result;
    }

    if (HasMultipleSelectedRoots(request))
    {
        AddDiagnostic(
            result,
            "prefab-multi-root-selection",
            "Prefab creation currently requires exactly one root selection.");
        return result;
    }

    if (!request.destinationAssetId.IsValid())
    {
        AddDiagnostic(result, "prefab-invalid-destination-asset", "Prefab creation requires a valid destination asset id.");
        return result;
    }

    const auto prefab = NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(*request.selectedRoot);
    result.prefabSourceText = NLS::Engine::Serialize::ObjectGraphWriter::Write(prefab.graph);
    auto resolvedAssets = NLS::Engine::Assets::BuildPrefabResolvedAssetsFromReferences(prefab.graph);

    const auto importResult = NLS::Engine::Assets::ImportPrefabArtifact(
        result.prefabSourceText,
        request.destinationAssetId,
        std::move(resolvedAssets));
    if (importResult.diagnostics.HasErrors())
    {
        result.status = PrefabEditorOperationStatus::Failed;
        AddSerializationDiagnostics(result, importResult.diagnostics);
        return result;
    }

    result.artifact = importResult.artifact;
    result.createdPrefabAssetId = request.destinationAssetId;
    result.createdPrefabPath = request.destinationPath;
    result.status = PrefabEditorOperationStatus::Committed;
    return result;
}

PrefabEditorOperationResult PrefabEditorWorkflow::InstantiatePrefab(
    const InstantiatePrefabRequest& request,
    NLS::Engine::SceneSystem::Scene& scene) const
{
    PrefabEditorOperationResult result;
    result.status = PrefabEditorOperationStatus::Rejected;

    const auto* prefab = request.constPrefab != nullptr
        ? request.constPrefab
        : request.prefab;
    if (!prefab)
    {
        AddDiagnostic(result, "prefab-missing-artifact", "Prefab instantiation requires a prefab artifact.");
        return result;
    }

    if (!request.prefabAssetId.IsValid())
    {
        AddDiagnostic(result, "prefab-invalid-asset-id", "Prefab instantiation requires a valid prefab asset id.");
        return result;
    }

    NLS::Engine::Serialize::LoadPolicy loadPolicy;
    loadPolicy.deferAssetReferenceResolution = request.deferAssetReferenceResolution;
    loadPolicy.synchronousAssetReferencePrewarm = request.synchronousAssetReferencePrewarm;
    loadPolicy.skipDeferredAssetReferenceCacheLookup = request.skipDeferredAssetReferenceCacheLookup;
    auto instantiateResult = NLS::Engine::Assets::InstantiatePrefabArtifact(*prefab, scene, loadPolicy);
    if (instantiateResult.diagnostics.HasErrors())
    {
        result.status = PrefabEditorOperationStatus::Failed;
        AddSerializationDiagnostics(result, instantiateResult.diagnostics);
        return result;
    }

    PrefabInstanceRecord instance;
    instance.prefabAssetId = request.prefabAssetId;
    instance.sceneAssetId = request.sceneAssetId;
    instance.prefabSubAssetKey = request.prefabSubAssetKey;
    instance.generatedReadOnly = prefab->generatedModelPrefab;
    instance.instanceRoot = instantiateResult.root;
    instance.sourceGraph = prefab->graph;
    instance.sourceToInstance = std::move(instantiateResult.sourceToInstance);
    instance.sourceByInstanceObject.clear();
    for (const auto& mapping : instantiateResult.sourceByInstanceObject)
        instance.sourceByInstanceObject.emplace(mapping.first, mapping.second);
    instance.preservedAssetReferences = NLS::Engine::Assets::CollectPrefabAssetReferences(prefab->graph);
    instance.preservedResolvedAssets = prefab->resolvedAssets;
    instance.nestedInstances = BuildNestedInstanceRecords(
        *prefab,
        instance.preservedAssetReferences,
        request.sceneAssetId,
        instance.instanceRoot);
    if (instance.instanceRoot)
    {
        if (const auto* rootRecord = FindObjectRecord(prefab->graph, prefab->graph.root))
            MapInstanceHierarchy(*prefab, *rootRecord, *instance.instanceRoot, instance);
    }

    result.instance = std::move(instance);
    if (request.sceneAssetId.IsValid())
    {
        AddDependencyChange(
            result,
            NLS::Core::Assets::AssetDependencyChangeKind::Add,
            request.sceneAssetId,
            NLS::Core::Assets::MakePrefabOverrideTargetDependency(
                request.prefabAssetId,
                request.prefabSubAssetKey));
    }
    result.status = PrefabEditorOperationStatus::Committed;
    return result;
}

PrefabEditorOperationResult PrefabEditorWorkflow::ConnectExistingPrefabInstance(
    const InstantiatePrefabRequest& request,
    GameObject& root) const
{
    PrefabEditorOperationResult result;
    result.status = PrefabEditorOperationStatus::Rejected;

    const auto* prefab = request.constPrefab != nullptr
        ? request.constPrefab
        : request.prefab;
    if (!prefab)
    {
        AddDiagnostic(result, "prefab-missing-artifact", "Connecting a prefab instance requires a prefab artifact.");
        return result;
    }

    if (!request.prefabAssetId.IsValid())
    {
        AddDiagnostic(result, "prefab-invalid-asset-id", "Connecting a prefab instance requires a valid prefab asset id.");
        return result;
    }

    const auto validation = prefab->Validate();
    if (validation.HasErrors())
    {
        result.status = PrefabEditorOperationStatus::Failed;
        AddSerializationDiagnostics(result, validation);
        return result;
    }

    PrefabInstanceRecord instance;
    instance.prefabAssetId = request.prefabAssetId;
    instance.sceneAssetId = request.sceneAssetId;
    instance.prefabSubAssetKey = request.prefabSubAssetKey;
    instance.generatedReadOnly = prefab->generatedModelPrefab;
    instance.instanceRoot = &root;
    instance.sourceGraph = prefab->graph;
    instance.preservedAssetReferences = NLS::Engine::Assets::CollectPrefabAssetReferences(prefab->graph);
    instance.preservedResolvedAssets = prefab->resolvedAssets;
    instance.nestedInstances = BuildNestedInstanceRecords(
        *prefab,
        instance.preservedAssetReferences,
        request.sceneAssetId,
        instance.instanceRoot);

    if (!RebuildExistingInstanceFromPrefab(*prefab, root, instance))
    {
        AddDiagnostic(
            result,
            "prefab-connect-root-mismatch",
            "Existing scene object could not be connected to the saved prefab root.");
        result.status = PrefabEditorOperationStatus::Failed;
        return result;
    }

    result.instance = std::move(instance);
    if (request.sceneAssetId.IsValid())
    {
        AddDependencyChange(
            result,
            NLS::Core::Assets::AssetDependencyChangeKind::Add,
            request.sceneAssetId,
            NLS::Core::Assets::MakePrefabOverrideTargetDependency(
                request.prefabAssetId,
                request.prefabSubAssetKey));
    }
    result.status = PrefabEditorOperationStatus::Committed;
    return result;
}

std::vector<PrefabOverrideRecord> PrefabEditorWorkflow::DiscoverOverrides(
    const NLS::Engine::Assets::PrefabArtifact& prefab,
    const PrefabInstanceRecord& instance) const
{
    std::vector<PrefabOverrideRecord> overrides;
    if (!instance.instanceRoot)
        return overrides;

    const auto* rootRecord = FindObjectRecord(prefab.graph, prefab.graph.root);
    if (rootRecord)
        DiscoverOverridesRecursive(prefab, instance, *rootRecord, *instance.instanceRoot, overrides);

    AddLocalPatchOverrides(instance, overrides);

    return overrides;
}

PrefabEditorOperationResult PrefabEditorWorkflow::ApplySelectedOverride(
    NLS::Engine::Assets::PrefabArtifact& prefab,
    const PrefabOverrideRecord& overrideRecord) const
{
    return ApplyAllOverrides(prefab, {overrideRecord});
}

PrefabEditorOperationResult PrefabEditorWorkflow::ApplyAllOverrides(
    NLS::Engine::Assets::PrefabArtifact& prefab,
    const std::vector<PrefabOverrideRecord>& overrides) const
{
    PrefabEditorOperationResult result;
    result.status = PrefabEditorOperationStatus::Failed;

    auto candidate = prefab;
    for (const auto& overrideRecord : overrides)
    {
        if (!ApplyOverrideToArtifact(candidate, overrideRecord))
        {
            AddDiagnostic(
                result,
                "prefab-apply-override-failed",
                "Prefab override could not be applied to the editable prefab artifact.");
            return result;
        }
    }

    const auto diagnostics = candidate.Validate();
    if (diagnostics.HasErrors())
    {
        AddValidationDiagnostics(result, diagnostics);
        return result;
    }

    prefab = std::move(candidate);
    result.artifact = prefab;
    AddDependencyRefreshRequest(
        result,
        NLS::Core::Assets::MakePrefabBaseDependency(prefab.assetId, {}));
    AddDependencyRefreshRequest(
        result,
        NLS::Core::Assets::MakeNestedPrefabDependency(prefab.assetId, {}));
    AddDependencyRefreshRequest(
        result,
        NLS::Core::Assets::MakePrefabOverrideTargetDependency(prefab.assetId, {}));
    AddDependencyRefreshRequest(
        result,
        {NLS::Core::Assets::AssetDependencyKind::SourceAssetGuid, prefab.assetId.ToString(), {}});
    result.status = PrefabEditorOperationStatus::Committed;
    return result;
}

PrefabEditorOperationResult PrefabEditorWorkflow::RevertSelectedOverride(
    PrefabInstanceRecord& instance,
    const NLS::Engine::Serialize::PatchOperation& patch) const
{
    PrefabEditorOperationResult result;
    result.status = PrefabEditorOperationStatus::Rejected;

    const auto localPatch = std::find_if(
        instance.localPatches.begin(),
        instance.localPatches.end(),
        [&patch](const PatchOperation& candidate)
        {
            return IsSamePatchIdentity(candidate, patch);
        });
    if (localPatch == instance.localPatches.end())
    {
        AddDiagnostic(
            result,
            "prefab-revert-override-not-found",
            "Prefab instance does not contain the selected override.");
        return result;
    }

    PrefabArtifact sourcePrefab {instance.prefabAssetId, instance.SourceGraph()};
    if (!RevertLivePropertyToPrefabValue(sourcePrefab, instance, patch))
    {
        AddDiagnostic(
            result,
            "prefab-revert-live-state-failed",
            "Prefab instance live state could not be restored for the selected override.");
        return result;
    }
    if (!RevertLiveStructuralPatch(instance, patch))
    {
        AddDiagnostic(
            result,
            "prefab-revert-live-structure-failed",
            "Prefab instance live structure could not be restored for the selected override.");
        return result;
    }

    instance.localPatches.erase(
        std::remove_if(
            instance.localPatches.begin(),
            instance.localPatches.end(),
            [&patch](const PatchOperation& candidate)
            {
                return IsSamePatchIdentity(candidate, patch);
            }),
        instance.localPatches.end());

    result.instance = instance;
    if (instance.sceneAssetId.IsValid() && instance.prefabAssetId.IsValid())
    {
        AddDependencyRefreshRequest(
            result,
            {NLS::Core::Assets::AssetDependencyKind::SourceAssetGuid, instance.sceneAssetId.ToString(), {}});
    }
    result.status = PrefabEditorOperationStatus::Committed;
    return result;
}

PrefabEditorOperationResult PrefabEditorWorkflow::RevertAllOverrides(PrefabInstanceRecord& instance) const
{
    PrefabEditorOperationResult result;
    const bool hadOverrides = !instance.localPatches.empty();
    const auto patches = instance.localPatches;
    PrefabArtifact sourcePrefab {instance.prefabAssetId, instance.SourceGraph()};
    for (const auto& patch : patches)
    {
        if (!RevertLivePropertyToPrefabValue(sourcePrefab, instance, patch))
        {
            result.status = PrefabEditorOperationStatus::Rejected;
            AddDiagnostic(
                result,
                "prefab-revert-live-state-failed",
                "Prefab instance live state could not be restored for one or more overrides.");
            return result;
        }
        if (!RevertLiveStructuralPatch(instance, patch))
        {
            result.status = PrefabEditorOperationStatus::Rejected;
            AddDiagnostic(
                result,
                "prefab-revert-live-structure-failed",
                "Prefab instance live structure could not be restored for one or more overrides.");
            return result;
        }
    }
    instance.localPatches.clear();
    result.instance = instance;
    if (hadOverrides && instance.sceneAssetId.IsValid() && instance.prefabAssetId.IsValid())
    {
        AddDependencyRefreshRequest(
            result,
            {NLS::Core::Assets::AssetDependencyKind::SourceAssetGuid, instance.sceneAssetId.ToString(), {}});
    }
    result.status = PrefabEditorOperationStatus::Committed;
    return result;
}

PrefabEditorOperationResult PrefabEditorWorkflow::OpenPrefabStage(const OpenPrefabStageRequest& request) const
{
    PrefabEditorOperationResult result;
    result.status = PrefabEditorOperationStatus::Rejected;

    if (!request.prefab)
    {
        AddDiagnostic(result, "prefab-stage-missing-artifact", "Opening a prefab stage requires a prefab artifact.");
        return result;
    }

    if (!request.prefabAssetId.IsValid())
    {
        AddDiagnostic(result, "prefab-stage-invalid-asset-id", "Opening a prefab stage requires a valid prefab asset id.");
        return result;
    }

    std::optional<NLS::Base::Profiling::PerformanceStageScope> openStageScope;
    if (NLS::Base::Profiling::PerformanceStageScope::GetActiveStats() != nullptr)
    {
        openStageScope.emplace(
            NLS::Base::Profiling::PerformanceStageDomain::Prefab,
            "OpenStage",
            NLS::Base::Profiling::PerformanceStageThread::Main);
        openStageScope->AddCounter("objectCount", request.prefab->graph.objects.size());
        openStageScope->AddCounter("instantiationGraphCopyCount", 0u);
    }

    auto stageScene = std::make_unique<NLS::Engine::SceneSystem::Scene>();
    const auto instantiated = NLS::Engine::Assets::InstantiatePrefabArtifact(
        std::as_const(*request.prefab),
        *stageScene);
    if (instantiated.diagnostics.HasErrors() || !instantiated.root)
    {
        result.status = PrefabEditorOperationStatus::Failed;
        AddSerializationDiagnostics(result, instantiated.diagnostics);
        if (!instantiated.root)
        {
            AddDiagnostic(
                result,
                "prefab-stage-root-missing",
                "Prefab stage could not instantiate the prefab root.");
        }
        return result;
    }

    PrefabStageState stage;
    stage.prefabAssetId = request.prefabAssetId;
    stage.prefabAssetPath = request.prefabAssetPath;
    stage.prefabSubAssetKey = request.prefabSubAssetKey;
    stage.loaded = true;
    stage.generatedReadOnly = request.generatedReadOnly || request.prefab->generatedModelPrefab;
    stage.editable = !stage.generatedReadOnly;
    stage.openedGraph = request.prefab->graph;
    if (openStageScope)
        openStageScope->AddCounter("openedGraphSnapshotCopyCount", 1u);
    stage.stageRoot = instantiated.root;
    stage.stageScene = std::move(stageScene);

    result.stage = std::move(stage);
    result.status = PrefabEditorOperationStatus::Committed;
    return result;
}

void PrefabEditorWorkflow::MarkStageDirty(PrefabStageState& stage) const
{
    if (stage.stageScene)
        stage.stageScene->MarkRenderContentChanged();
    stage.dirty = true;
}

PrefabEditorOperationResult PrefabEditorWorkflow::SavePrefabStage(
    PrefabStageState& stage,
    NLS::Engine::Assets::PrefabArtifact& prefab,
    PrefabInstanceRegistry* instanceRegistry) const
{
    PrefabEditorOperationResult result;
    result.status = PrefabEditorOperationStatus::Rejected;

    if (!stage.loaded || !stage.stageRoot)
    {
        AddDiagnostic(result, "prefab-stage-not-loaded", "Prefab stage is not loaded.");
        return result;
    }

    if (!stage.editable || stage.generatedReadOnly || prefab.generatedModelPrefab)
    {
        AddDiagnostic(
            result,
            "prefab-stage-read-only",
            "Generated prefab stages are read-only; create a variant or unpack before saving edits.");
        return result;
    }

    if (!stage.stageRoot)
    {
        AddDiagnostic(result, "prefab-stage-root-missing", "Prefab stage has no root object to save.");
        return result;
    }

    auto saved = NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(*stage.stageRoot).graph;
    saved.documentId = stage.openedGraph.documentId;
    saved.basePrefab = stage.openedGraph.basePrefab;
    PreserveExistingObjectIdsFromOpenedGraph(saved, stage.openedGraph);

    auto candidate = prefab;
    candidate.graph = std::move(saved);
    NLS::Engine::Assets::RefreshPrefabResolvedAssetsFromReferences(candidate);
    if (candidate.graph.basePrefab.has_value())
    {
        candidate.baseChain.clear();
        candidate.baseChain.push_back(
            NLS::Core::Assets::AssetId(candidate.graph.basePrefab->guid));
    }
    const auto diagnostics = candidate.Validate();
    if (diagnostics.HasErrors())
    {
        result.status = PrefabEditorOperationStatus::Failed;
        AddValidationDiagnostics(result, diagnostics);
        return result;
    }

    prefab = std::move(candidate);
    if (instanceRegistry)
    {
        auto instances = instanceRegistry->FindInstancesForPrefab(prefab.assetId, stage.prefabSubAssetKey);
        for (auto* instance : instances)
        {
            if (instance && RefreshConnectedInstanceFromPrefab(*instance, prefab))
                continue;

            result.status = PrefabEditorOperationStatus::Failed;
            AddDiagnostic(
                result,
                "prefab-stage-instance-refresh-failed",
                "A connected prefab instance could not be refreshed after saving the prefab stage.");
            return result;
        }
    }

    stage.openedGraph = prefab.graph;
    stage.dirty = false;
    result.artifact = prefab;
    AddDependencyRefreshRequest(
        result,
        NLS::Core::Assets::MakePrefabBaseDependency(prefab.assetId, {}));
    AddDependencyRefreshRequest(
        result,
        NLS::Core::Assets::MakeNestedPrefabDependency(prefab.assetId, {}));
    AddDependencyRefreshRequest(
        result,
        NLS::Core::Assets::MakePrefabOverrideTargetDependency(prefab.assetId, {}));
    AddDependencyRefreshRequest(
        result,
        {NLS::Core::Assets::AssetDependencyKind::SourceAssetGuid, prefab.assetId.ToString(), {}});
    result.status = PrefabEditorOperationStatus::Committed;
    return result;
}

PrefabEditorOperationResult PrefabEditorWorkflow::DiscardPrefabStage(PrefabStageState& stage) const
{
    PrefabEditorOperationResult result;
    if (!stage.loaded)
    {
        AddDiagnostic(result, "prefab-stage-not-loaded", "Prefab stage is not loaded.");
        result.status = PrefabEditorOperationStatus::Rejected;
        return result;
    }

    auto stageScene = std::make_unique<NLS::Engine::SceneSystem::Scene>();
    NLS::Engine::Assets::PrefabArtifact artifact;
    artifact.assetId = stage.prefabAssetId;
    artifact.graph = stage.openedGraph;
    const auto instantiated = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, *stageScene);
    if (instantiated.diagnostics.HasErrors() || !instantiated.root)
    {
        result.status = PrefabEditorOperationStatus::Failed;
        AddSerializationDiagnostics(result, instantiated.diagnostics);
        return result;
    }

    stage.stageRoot = instantiated.root;
    stage.stageScene = std::move(stageScene);
    stage.loaded = true;
    stage.dirty = false;
    result.status = PrefabEditorOperationStatus::Committed;
    return result;
}

PrefabEditorOperationResult PrefabEditorWorkflow::CreateEditableVariant(
    const CreateEditableVariantRequest& request) const
{
    PrefabEditorOperationResult result;
    result.status = PrefabEditorOperationStatus::Rejected;

    if (!request.basePrefab)
    {
        AddDiagnostic(result, "prefab-variant-missing-base", "Creating a prefab variant requires a base prefab artifact.");
        return result;
    }

    if (!request.basePrefabAssetId.IsValid() || !request.newVariantAssetId.IsValid())
    {
        AddDiagnostic(result, "prefab-variant-invalid-asset-id", "Creating a prefab variant requires valid base and destination asset ids.");
        return result;
    }

    if (request.destinationExists)
    {
        AddDiagnostic(
            result,
            "prefab-variant-destination-conflict",
            "Prefab variant destination already exists.");
        return result;
    }

    NLS::Engine::Assets::PrefabArtifact variant;
    variant.assetId = request.newVariantAssetId;
    variant.graph = request.basePrefab->graph;
    variant.graph.documentId = NLS::Guid::NewDeterministic(
        "Prefab.Variant.Document:" +
        request.newVariantAssetId.GetGuid().ToString() + ":" +
        request.destinationPath.generic_string());
    variant.graph.basePrefab = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(request.basePrefabAssetId.GetGuid()),
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(
            request.basePrefabAssetId.GetGuid(),
            request.basePrefabSubAssetKey),
        request.basePrefabSubAssetKey);
    variant.baseChain = {request.basePrefabAssetId};
    variant.resolvedAssets = request.basePrefab->resolvedAssets;

    const auto diagnostics = variant.Validate();
    if (diagnostics.HasErrors())
    {
        result.status = PrefabEditorOperationStatus::Failed;
        AddValidationDiagnostics(result, diagnostics);
        return result;
    }

    result.artifact = std::move(variant);
    result.createdPrefabAssetId = request.newVariantAssetId;
    result.createdPrefabPath = request.destinationPath;
    result.prefabSourceText = NLS::Engine::Serialize::ObjectGraphWriter::Write(result.artifact->graph);
    AddDependencyChange(
        result,
        NLS::Core::Assets::AssetDependencyChangeKind::Add,
        request.newVariantAssetId,
        NLS::Core::Assets::MakePrefabBaseDependency(
            request.basePrefabAssetId,
            request.basePrefabSubAssetKey));
    AddDependencyRefreshRequest(
        result,
        NLS::Core::Assets::MakePrefabBaseDependency(request.basePrefabAssetId, request.basePrefabSubAssetKey));
    result.status = PrefabEditorOperationStatus::Committed;
    return result;
}

PrefabEditorOperationResult PrefabEditorWorkflow::ValidateNestedPrefabs(
    const std::vector<NLS::Engine::Assets::PrefabArtifact>& prefabs) const
{
    PrefabEditorOperationResult result;
    const auto diagnostics = NLS::Engine::Assets::ValidateNestedPrefabDependencies(prefabs);
    if (diagnostics.HasErrors())
    {
        result.status = PrefabEditorOperationStatus::Failed;
        for (const auto& diagnostic : diagnostics.GetItems())
        {
            result.diagnostics.push_back({
                "prefab-nested-dependency-diagnostic",
                diagnostic.GetMessage()
            });
        }
        return result;
    }

    result.status = PrefabEditorOperationStatus::Committed;
    return result;
}

PrefabEditorOperationResult PrefabEditorWorkflow::UnpackPrefabInstance(PrefabInstanceRecord& instance) const
{
    PrefabEditorOperationResult result;
    result.status = PrefabEditorOperationStatus::Rejected;

    if (!instance.instanceRoot)
    {
        AddDiagnostic(result, "prefab-unpack-missing-root", "Prefab unpack requires an instance root.");
        return result;
    }

    PrefabUnpackResult unpack;
    unpack.root = instance.instanceRoot;
    for (const auto& mapping : instance.sourceToInstance)
        unpack.sceneOwnedObjects.push_back(mapping.second);

    unpack.preservedAssetReferences = instance.preservedAssetReferences;
    const auto previousSceneAssetId = instance.sceneAssetId;
    const auto previousPrefabAssetId = instance.prefabAssetId;
    const auto previousPrefabSubAssetKey = instance.prefabSubAssetKey;

    instance.prefabAssetId = {};
    instance.prefabSubAssetKey.clear();
    instance.unpacked = true;
    instance.sourceToInstance.clear();
    instance.sourceByInstanceObject.clear();
    instance.localPatches.clear();
    instance.preservedAssetReferences.clear();
    instance.preservedResolvedAssets.clear();
    instance.nestedInstances.clear();

    result.unpack = std::move(unpack);
    result.instance = instance;
    if (previousSceneAssetId.IsValid())
    {
        AddDependencyRefreshRequest(
            result,
            {NLS::Core::Assets::AssetDependencyKind::SourceAssetGuid, previousSceneAssetId.ToString(), {}});
    }
    if (previousSceneAssetId.IsValid() && previousPrefabAssetId.IsValid())
    {
        AddDependencyChange(
            result,
            NLS::Core::Assets::AssetDependencyChangeKind::Remove,
            previousSceneAssetId,
            NLS::Core::Assets::MakePrefabOverrideTargetDependency(
                previousPrefabAssetId,
                previousPrefabSubAssetKey));
    }
    result.status = PrefabEditorOperationStatus::Committed;
    return result;
}

PrefabEditorOperationResult PrefabEditorWorkflow::AggregatePrefabEditorDiagnostics(
    std::initializer_list<std::reference_wrapper<const PrefabEditorOperationResult>> operationResults,
    const std::vector<NLS::Engine::Assets::PrefabArtifact>& artifacts) const
{
    PrefabEditorOperationResult result;
    result.status = PrefabEditorOperationStatus::Committed;

    for (const auto& operationResultRef : operationResults)
    {
        const auto& operationResult = operationResultRef.get();
        if (operationResult.status != PrefabEditorOperationStatus::Committed)
            result.status = PrefabEditorOperationStatus::Failed;

        result.diagnostics.insert(
            result.diagnostics.end(),
            operationResult.diagnostics.begin(),
            operationResult.diagnostics.end());
    }

    for (const auto& artifact : artifacts)
    {
        const auto validationDiagnostics = artifact.Validate();
        if (validationDiagnostics.HasErrors())
        {
            result.status = PrefabEditorOperationStatus::Failed;
            AddValidationDiagnostics(result, validationDiagnostics);
        }

        AddArtifactDiagnostics(result, artifact);
    }

    const auto nestedDiagnostics = NLS::Engine::Assets::ValidateNestedPrefabDependencies(artifacts);
    if (nestedDiagnostics.HasErrors())
    {
        result.status = PrefabEditorOperationStatus::Failed;
        for (const auto& diagnostic : nestedDiagnostics.GetItems())
        {
            result.diagnostics.push_back({
                "prefab-nested-dependency-diagnostic",
                diagnostic.GetMessage()
            });
        }
    }

    if (!result.diagnostics.empty())
        result.status = PrefabEditorOperationStatus::Failed;

    return result;
}
}
