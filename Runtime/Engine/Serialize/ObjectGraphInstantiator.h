#pragma once

#include <memory>
#include <optional>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Components/Component.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Components/TransformComponent.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ServiceLocator.h"
#include "GameObject.h"
#include "Reflection/Field.h"
#include "Rendering/ExternalReflection.h"
#include "Rendering/Resources/Material.h"
#include "Reflection/TypeCreator.h"
#include "SceneSystem/Scene.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/ObjectGraphPPtr.h"
#include "Serialize/ObjectReferenceResolver.h"
#include "Serialize/PrefabDocument.h"

namespace NLS::Engine::Serialize
{
    enum class UnknownTypePolicy
    {
        Preserve,
        Fail
    };

    enum class MissingAssetPolicy
    {
        Preserve,
        Fail
    };

    enum class InvalidReferencePolicy
    {
        Preserve,
        Fail
    };

    struct LoadPolicy
    {
        UnknownTypePolicy unknownTypePolicy = UnknownTypePolicy::Fail;
        MissingAssetPolicy missingAssetPolicy = MissingAssetPolicy::Preserve;
        InvalidReferencePolicy invalidReferencePolicy = InvalidReferencePolicy::Fail;
        bool deferAssetReferenceResolution = false;
    };

    struct DocumentAnalysisResult
    {
        SerializationDiagnosticList diagnostics;
    };

    struct SceneInstantiationResult
    {
        std::unique_ptr<SceneSystem::Scene> scene;
        SerializationDiagnosticList diagnostics;
    };

    struct InstantiationProgress
    {
        float normalizedProgress = 0.0f;
        std::string message;
    };

    using InstantiationProgressCallback = std::function<void(const InstantiationProgress&)>;

    class ObjectGraphInstantiator
    {
    public:
        static DocumentAnalysisResult AnalyzeDocument(const ObjectGraphDocument& document, const LoadPolicy& policy)
        {
            DocumentAnalysisResult result;
            result.diagnostics = document.Validate();

            AnalyzeObjectTypes(document, policy, result.diagnostics);
            AnalyzeReflectedObjectReferenceShapes(document, policy, result.diagnostics);
            AnalyzeAssetReferences(document, policy, result.diagnostics);
            return result;
        }

        static std::unique_ptr<SceneSystem::Scene> InstantiateScene(const ObjectGraphDocument& document)
        {
            return InstantiateSceneStrict(document);
        }

        static SceneInstantiationResult InstantiateScene(const ObjectGraphDocument& document, const LoadPolicy& policy)
        {
            return InstantiateScene(document, policy, {});
        }

        static SceneInstantiationResult InstantiateScene(
            const ObjectGraphDocument& document,
            const LoadPolicy& policy,
            const InstantiationProgressCallback& progressCallback)
        {
            SceneInstantiationResult result;
            result.diagnostics = AnalyzeDocument(document, policy).diagnostics;
            if (result.diagnostics.HasErrors())
                return result;

            result.scene = InstantiateSceneStrict(document, progressCallback, policy);
            if (!result.scene)
            {
                result.diagnostics.Add({
                    SerializationDiagnosticCode::MissingObject,
                    SerializationDiagnosticSeverity::Error,
                    "Object graph scene could not be instantiated."
                });
            }
            return result;
        }

        static std::unique_ptr<SceneSystem::Scene> InstantiateSceneStrict(const ObjectGraphDocument& document)
        {
            return InstantiateSceneStrict(document, {});
        }

        static std::unique_ptr<SceneSystem::Scene> InstantiateSceneStrict(
            const ObjectGraphDocument& document,
            const InstantiationProgressCallback& progressCallback)
        {
            return InstantiateSceneStrict(document, progressCallback, {});
        }

        static std::unique_ptr<SceneSystem::Scene> InstantiateSceneStrict(
            const ObjectGraphDocument& document,
            const InstantiationProgressCallback& progressCallback,
            const LoadPolicy& policy)
        {
            if (document.Validate().HasErrors())
                return nullptr;

            const auto* sceneRecord = FindRecord(document, document.root);
            if (!sceneRecord || !RecordTypeMatches<NLS::Engine::SceneSystem::Scene>(*sceneRecord))
                return nullptr;

            auto scene = std::make_unique<SceneSystem::Scene>();
            InstanceContext context;
            context.document = &document;

            const auto* gameObjects = FindProperty(*sceneRecord, "gameObjects");
            if (!gameObjects || gameObjects->value.GetKind() != PropertyValue::Kind::Array)
                return scene;

            const auto& gameObjectReferences = gameObjects->value.GetArray();
            const auto gameObjectCount = std::max<size_t>(gameObjectReferences.size(), 1u);
            size_t createdGameObjectCount = 0;
            for (const auto& reference : gameObjectReferences)
            {
                const auto objectId = ResolveObjectId(document, reference);
                if (!objectId.has_value())
                    continue;

                const auto* record = FindRecord(document, *objectId);
                if (!record ||
                    !IsInstantiableRecordState(record->state) ||
                    !RecordTypeMatches<GameObject>(*record))
                    continue;

                auto* gameObject = CreateGameObject(*record);
                if (!gameObject)
                    continue;

                context.gameObjects.emplace(record->id, gameObject);
                scene->AddGameObject(gameObject);
                ++createdGameObjectCount;
                ReportProgress(
                    progressCallback,
                    0.25f + 0.20f * (static_cast<float>(createdGameObjectCount) / static_cast<float>(gameObjectCount)),
                    "Creating GameObject: " + gameObject->GetName());
            }

            const auto objectCount = std::max<size_t>(document.objects.size(), 1u);
            size_t appliedObjectCount = 0;
            for (const auto& object : document.objects)
            {
                ++appliedObjectCount;
                if (!IsInstantiableRecordState(object.state))
                    continue;

                if (!RecordTypeMatches<GameObject>(object))
                    continue;

                auto* gameObject = FindGameObject(context, object.id);
                if (!gameObject)
                    continue;

                ReportProgress(
                    progressCallback,
                    0.45f + 0.30f * (static_cast<float>(appliedObjectCount) / static_cast<float>(objectCount)),
                    "Restoring components: " + gameObject->GetName());
                ApplyGameObjectState(*gameObject, object);
                InstantiateComponents(document, object, *gameObject, context, policy);
            }

            size_t resolvedObjectCount = 0;
            for (const auto& object : document.objects)
            {
                ++resolvedObjectCount;
                if (!IsInstantiableRecordState(object.state))
                    continue;

                if (RecordTypeMatches<GameObject>(object))
                {
                    ReportProgress(
                        progressCallback,
                        0.75f + 0.15f * (static_cast<float>(resolvedObjectCount) / static_cast<float>(objectCount)),
                        "Restoring hierarchy");
                    ResolveParent(context, object);
                }
            }

            ReportProgress(progressCallback, 0.92f, "Rebuilding scene runtime caches");
            scene->RebuildRuntimeCachesAfterLoad();
            return scene;
        }

        static PrefabInstantiationResult InstantiatePrefab(const PrefabDocument& prefab, SceneSystem::Scene& scene)
        {
            return InstantiatePrefab(prefab, scene, {});
        }

        static PrefabInstantiationResult InstantiatePrefab(
            const PrefabDocument& prefab,
            SceneSystem::Scene& scene,
            const LoadPolicy& policy)
        {
            auto graph = prefab.graph;
            ApplyOverrides(graph);

            PrefabInstantiationResult result;
            AnalyzeObjectTypes(graph, policy, result.diagnostics);
            AnalyzeReflectedObjectReferenceShapes(graph, policy, result.diagnostics);
            AnalyzeAssetReferences(graph, policy, result.diagnostics);
            if (result.diagnostics.HasErrors())
                return result;

            InstanceContext context;
            context.document = &graph;

            for (const auto& object : graph.objects)
            {
                if (!IsInstantiableRecordState(object.state))
                    continue;

                if (!RecordTypeMatches<GameObject>(object))
                    continue;

                auto* gameObject = CreateGameObject(object);
                if (!gameObject)
                    continue;

                const auto instanceId = ObjectId(NLS::Guid::NewDeterministic("Prefab.Instance:" + object.id.GetGuid().ToString()));
                result.sourceToInstance.emplace(object.id, instanceId);
                result.sourceByInstanceObject.emplace(gameObject, object.id);
                context.gameObjects.emplace(object.id, gameObject);
            }

            for (const auto& object : graph.objects)
            {
                if (!IsInstantiableRecordState(object.state))
                    continue;

                if (!RecordTypeMatches<GameObject>(object))
                    continue;

                auto* gameObject = FindGameObject(context, object.id);
                if (!gameObject)
                    continue;

                ApplyGameObjectState(*gameObject, object);
                InstantiateComponents(graph, object, *gameObject, context, policy);
                RegisterComponentMappings(object, result, graph);
            }

            for (const auto& object : graph.objects)
            {
                if (!IsInstantiableRecordState(object.state))
                    continue;

                if (RecordTypeMatches<GameObject>(object))
                    ResolveParent(context, object);
            }

            result.root = FindGameObject(context, graph.root);
            if (result.root)
                scene.AddGameObject(result.root);

            scene.RebuildRuntimeCachesAfterLoad();
            return result;
        }

        static SerializationDiagnosticList ValidatePrefab(const PrefabDocument& prefab)
        {
            SerializationDiagnosticList diagnostics = prefab.graph.Validate();
            for (const auto& object : prefab.graph.objects)
            {
                if (object.state != ObjectRecordState::Stripped)
                    continue;

                diagnostics.Add({
                    SerializationDiagnosticCode::InvalidPrefabOverride,
                    SerializationDiagnosticSeverity::Error,
                    "Prefab source graphs cannot contain scene-only stripped object records."
                });
            }

            for (const auto& operation : prefab.graph.overrides)
            {
                if (!FindRecord(prefab.graph, operation.target))
                {
                    diagnostics.Add({
                        SerializationDiagnosticCode::InvalidPrefabOverride,
                        SerializationDiagnosticSeverity::Error,
                        "Prefab override targets a missing object."
                    });
                    continue;
                }

                if ((operation.type == PatchOperationType::InsertOwned ||
                     operation.type == PatchOperationType::RemoveOwned ||
                     operation.type == PatchOperationType::MoveOwned) &&
                    !FindRecord(prefab.graph, operation.object))
                {
                    diagnostics.Add({
                        SerializationDiagnosticCode::InvalidPrefabOverride,
                        SerializationDiagnosticSeverity::Error,
                        "Prefab override references a missing owned object."
                    });
                }
            }
            return diagnostics;
        }

    private:
        struct InstanceContext
        {
            const ObjectGraphDocument* document = nullptr;
            std::unordered_map<ObjectId, GameObject*> gameObjects;
            std::unordered_map<ObjectId, Components::Component*> components;
            std::unordered_map<std::string, Core::ResourceManagement::MeshManager::Mesh*> meshResourcesByNormalizedPath;
            std::unordered_map<std::string, Core::ResourceManagement::MaterialManager::Material*> materialResourcesByNormalizedPath;
            bool meshResourcePathIndexBuilt = false;
            bool materialResourcePathIndexBuilt = false;
        };

        static const ObjectRecord* FindRecord(const ObjectGraphDocument& document, const ObjectId& id)
        {
            for (const auto& object : document.objects)
            {
                if (object.id == id)
                    return &object;
            }
            return nullptr;
        }

        static const ObjectRecord* FindRecord(
            const ObjectGraphDocument& document,
            const ObjectIdentifier& reference)
        {
            const auto id = document.ResolveObjectReference(reference);
            return id.has_value() ? FindRecord(document, *id) : nullptr;
        }

        static std::optional<ObjectId> ResolveObjectId(
            const ObjectGraphDocument& document,
            const PropertyValue& value)
        {
            if (value.GetKind() == PropertyValue::Kind::OwnedReference)
                return value.GetObjectId();
            if (value.GetKind() == PropertyValue::Kind::ObjectReference)
                return document.ResolveObjectReference(value.GetObjectReference());
            return std::nullopt;
        }

        static const PropertyRecord* FindProperty(const ObjectRecord& record, const char* name)
        {
            for (const auto& property : record.properties)
            {
                if (property.name == name)
                    return &property;
            }
            return nullptr;
        }

        static PropertyRecord* FindMutableProperty(ObjectRecord& record, const char* name)
        {
            for (auto& property : record.properties)
            {
                if (property.name == name)
                    return &property;
            }
            return nullptr;
        }

        static ObjectRecord* FindMutableRecord(ObjectGraphDocument& document, const ObjectId& id)
        {
            for (auto& object : document.objects)
            {
                if (object.id == id)
                    return &object;
            }
            return nullptr;
        }

        static void ReportProgress(
            const InstantiationProgressCallback& callback,
            const float normalizedProgress,
            const std::string& message)
        {
            if (callback)
                callback({std::clamp(normalizedProgress, 0.0f, 1.0f), message});
        }

        static std::optional<std::string> ReadString(const ObjectRecord& record, const char* name)
        {
            const auto* property = FindProperty(record, name);
            if (!property || property->value.GetKind() != PropertyValue::Kind::String)
                return std::nullopt;
            return property->value.GetString();
        }

        static bool IsGameObjectGraphProperty(const std::string& name)
        {
            return name == "parent" || name == "components" || name == "children";
        }

        static bool IsNoGraphProperty(const std::string&)
        {
            return false;
        }

        template<typename ComponentType>
        static bool RecordTypeMatchesComponent(const ObjectRecord& record)
        {
            return RecordTypeMatches<ComponentType>(record);
        }

        template<typename ObjectType>
        static bool RecordTypeMatches(const ObjectRecord& record)
        {
            static const std::string typeName = NLS_TYPEOF(ObjectType).GetName();
            return record.typeName == typeName;
        }

        static bool IsDeferredAssetProperty(const ObjectRecord& record, const std::string& name)
        {
            if (RecordTypeMatchesComponent<Components::MeshFilter>(record))
                return name == "mesh";
            if (RecordTypeMatchesComponent<Components::MeshRenderer>(record))
                return name == "materials";
            return false;
        }

        static GameObject* CreateGameObject(const ObjectRecord& record)
        {
            const auto name = ReadString(record, "name").value_or(record.debugName);
            const auto tag = ReadString(record, "tag").value_or(std::string {});
            return new GameObject(name, tag);
        }

        static GameObject* FindGameObject(const InstanceContext& context, const ObjectId& id)
        {
            const auto found = context.gameObjects.find(id);
            return found != context.gameObjects.end() ? found->second : nullptr;
        }

        static void ApplyGameObjectState(GameObject& gameObject, const ObjectRecord& record)
        {
            ApplyReflectedFields(gameObject, record, IsGameObjectGraphProperty);
        }

        static void InstantiateComponents(
            const ObjectGraphDocument& document,
            const ObjectRecord& gameObjectRecord,
            GameObject& gameObject,
            InstanceContext& context,
            const LoadPolicy& policy = {})
        {
            const auto* components = FindProperty(gameObjectRecord, "components");
            if (!components || components->value.GetKind() != PropertyValue::Kind::Array)
                return;

            for (size_t index = 0; index < components->value.GetArray().size(); ++index)
            {
                const auto& reference = components->value.GetArray()[index];
                const auto componentId = ResolveObjectId(document, reference);
                if (!componentId.has_value())
                    continue;

                const auto* componentRecord = FindRecord(document, *componentId);
                if (!componentRecord)
                    continue;

                auto* component = EnsureComponent(gameObject, *componentRecord, index);
                if (!component)
                    continue;

                context.components.emplace(componentRecord->id, component);
                ApplyComponentState(*component, *componentRecord, context, policy);
                gameObject.MoveComponent(component, index);
            }
        }

        static Components::Component* EnsureComponent(GameObject& gameObject, const ObjectRecord& record, size_t index)
        {
            if (RecordTypeMatchesComponent<Components::TransformComponent>(record))
            {
                auto* transform = gameObject.GetTransform();
                if (transform)
                    transform->CreateBy(&gameObject);
                return transform;
            }

            const auto type = NLS::meta::Type::GetFromName(record.typeName);
            if (!type.IsValid() || !type.DerivesFrom(NLS_TYPEOF(Components::Component)))
                return nullptr;

            if (index < gameObject.GetComponents().size())
            {
                auto* existing = gameObject.GetComponents()[index].get();
                if (existing && existing->GetType() == type)
                    return existing;
            }

            return gameObject.AddComponent(type);
        }

        static void ApplyComponentState(
            Components::Component& component,
            const ObjectRecord& record,
            InstanceContext& context,
            const LoadPolicy& policy = {})
        {
            if (policy.deferAssetReferenceResolution)
                ApplyDeferredAssetReferenceHints(component, record, context);
            ApplyReflectedFields(component, record, IsNoGraphProperty, &policy, &context);
        }

        static void ApplyReflectedFields(
            NLS::Object& object,
            const ObjectRecord& record,
            bool (*isGraphProperty)(const std::string&),
            const LoadPolicy* policy = nullptr,
            InstanceContext* context = nullptr)
        {
            auto instance = NLS::meta::Variant(&object, NLS::meta::variant_policy::WrapObject {});
            const auto type = object.GetType();
            if (!type.IsValid())
                return;

            for (const auto& property : record.properties)
            {
                if (isGraphProperty && isGraphProperty(property.name))
                    continue;

                const auto field = type.GetField(property.name);
                if (!field.IsValid() || field.IsReadOnly())
                    continue;

                if (policy &&
                    policy->deferAssetReferenceResolution &&
                    (ContainsAssetReference(property.value) || IsDeferredAssetProperty(record, property.name)))
                {
                    continue;
                }

                const auto objectReferenceResult = TrySetUnityObjectReferenceField(instance, field, property.value);
                if (objectReferenceResult == UnityObjectReferenceApplyResult::Applied)
                {
                    if (context != nullptr)
                        ResolveRuntimeAssetReference(object, record, property, *context);
                    continue;
                }

                if (objectReferenceResult == UnityObjectReferenceApplyResult::ShapeMismatch)
                {
                    if (policy != nullptr &&
                        policy->invalidReferencePolicy == InvalidReferencePolicy::Preserve)
                    {
                        continue;
                    }

                    throw std::invalid_argument(
                        "Object graph property \"" + record.typeName + "." + property.name +
                        "\" must use a Unity-style object reference shape.");
                }

                const auto json = ConvertPropertyValue(property.value);
                auto value = field.GetType().DeserializeJson(NLS::Json(json));
                field.SetValue(instance, value);
            }
        }

        enum class UnityObjectReferenceApplyResult
        {
            NotObjectReferenceField,
            Applied,
            ShapeMismatch
        };

        static UnityObjectReferenceApplyResult TrySetUnityObjectReferenceField(
            NLS::meta::Variant& instance,
            const NLS::meta::Field& field,
            const PropertyValue& value)
        {
            const auto fieldType = field.GetType();
            if (fieldType.IsArray() && Internal::IsPPtrTypeName(fieldType.GetArrayType()))
            {
                switch (Internal::ApplyPPtrArrayOrThrow(instance, field, value))
                {
                case Internal::PPtrApplyResult::Applied:
                    return UnityObjectReferenceApplyResult::Applied;
                case Internal::PPtrApplyResult::ShapeMismatch:
                    return UnityObjectReferenceApplyResult::ShapeMismatch;
                }
            }

            if (Internal::IsPPtrTypeName(fieldType))
            {
                switch (Internal::ApplyPPtrValueOrThrow(instance, field, value))
                {
                case Internal::PPtrApplyResult::Applied:
                    return UnityObjectReferenceApplyResult::Applied;
                case Internal::PPtrApplyResult::ShapeMismatch:
                    return UnityObjectReferenceApplyResult::ShapeMismatch;
                }
            }

            return UnityObjectReferenceApplyResult::NotObjectReferenceField;
        }

        static void ResolveRuntimeAssetReference(
            NLS::Object& object,
            const ObjectRecord& record,
            const PropertyRecord& property,
            InstanceContext& context)
        {
            if (RecordTypeMatchesComponent<Components::MeshFilter>(record) && property.name == "mesh")
            {
                if (object.GetType() != NLS_TYPEOF(Components::MeshFilter) ||
                    property.value.GetKind() != PropertyValue::Kind::ObjectReference ||
                    !property.value.GetObjectReference().guid.IsValid())
                {
                    return;
                }
                auto* meshFilter = static_cast<Components::MeshFilter*>(&object);

                const auto path = ResolveAssetReferencePath(property.value.GetObjectReference());
                if (path.empty() ||
                    !Core::ServiceLocator::Contains<Core::ResourceManagement::MeshManager>())
                {
                    return;
                }

                auto& meshManager = NLS_SERVICE(Core::ResourceManagement::MeshManager);
                if (auto* mesh = FindCachedMeshResource(meshManager, path, context))
                    meshFilter->SetResolvedMeshFromReference(mesh);
                return;
            }

            if (RecordTypeMatchesComponent<Components::MeshRenderer>(record) && property.name == "materials")
            {
                if (object.GetType() != NLS_TYPEOF(Components::MeshRenderer) ||
                    property.value.GetKind() != PropertyValue::Kind::Array ||
                    !Core::ServiceLocator::Contains<Core::ResourceManagement::MaterialManager>())
                {
                    return;
                }
                auto* meshRenderer = static_cast<Components::MeshRenderer*>(&object);

                auto& materialManager = NLS_SERVICE(Core::ResourceManagement::MaterialManager);
                const auto& values = property.value.GetArray();
                for (size_t index = 0; index < values.size() && index < Components::MeshRenderer::kMaxMaterialCount; ++index)
                {
                    const auto& value = values[index];
                    if (value.GetKind() != PropertyValue::Kind::ObjectReference ||
                        !value.GetObjectReference().guid.IsValid())
                    {
                        continue;
                    }

                    const auto path = ResolveAssetReferencePath(value.GetObjectReference());
                    if (path.empty())
                        continue;

                    if (auto* material = FindCachedMaterialResource(materialManager, path, context))
                        meshRenderer->SetResolvedMaterialFromReference(static_cast<uint8_t>(index), *material);
                }
            }
        }

        static void ResolveParent(const InstanceContext& context, const ObjectRecord& record)
        {
            auto* child = FindGameObject(context, record.id);
            if (!child)
                return;

            const auto* parentProperty = FindProperty(record, "parent");
            if (!parentProperty || parentProperty->value.GetKind() != PropertyValue::Kind::ObjectReference)
                return;

            const auto parentId = context.document != nullptr
                ? context.document->ResolveObjectReference(parentProperty->value.GetObjectReference())
                : std::optional<ObjectId> {};
            auto* parent = parentId.has_value() ? FindGameObject(context, *parentId) : nullptr;
            if (parent)
                child->SetParent(*parent);
        }

        static void RegisterComponentMappings(
            const ObjectRecord& gameObjectRecord,
            PrefabInstantiationResult& result,
            const ObjectGraphDocument& graph)
        {
            const auto* components = FindProperty(gameObjectRecord, "components");
            if (!components || components->value.GetKind() != PropertyValue::Kind::Array)
                return;

            for (const auto& reference : components->value.GetArray())
            {
                const auto componentId = ResolveObjectId(graph, reference);
                if (!componentId.has_value())
                    continue;

                const auto* componentRecord = FindRecord(graph, *componentId);
                if (!componentRecord)
                    continue;

                result.sourceToInstance.emplace(
                    componentRecord->id,
                    ObjectId(NLS::Guid::NewDeterministic("Prefab.Instance:" + componentRecord->id.GetGuid().ToString())));
            }
        }

        static void ApplyOverrides(ObjectGraphDocument& graph)
        {
            for (const auto& operation : graph.overrides)
            {
                switch (operation.type)
                {
                case PatchOperationType::ReplaceProperty:
                    ApplyReplaceProperty(graph, operation);
                    break;
                case PatchOperationType::InsertOwned:
                    ApplyInsertOwned(graph, operation);
                    break;
                case PatchOperationType::RemoveOwned:
                    ApplyRemoveOwned(graph, operation);
                    break;
                case PatchOperationType::MoveOwned:
                    ApplyMoveOwned(graph, operation);
                    break;
                case PatchOperationType::RemoveObject:
                    ApplyRemoveObject(graph, operation);
                    break;
                default:
                    break;
                }
            }
        }

        static void ApplyReplaceProperty(ObjectGraphDocument& graph, const PatchOperation& operation)
        {
            auto* record = FindMutableRecord(graph, operation.target);
            if (!record)
                return;

            if (auto* property = FindMutableProperty(*record, operation.property.c_str()))
                property->value = operation.value;
            else
                record->properties.push_back({operation.property, operation.value});
        }

        static void ApplyInsertOwned(ObjectGraphDocument& graph, const PatchOperation& operation)
        {
            auto* record = FindMutableRecord(graph, operation.target);
            if (!record)
                return;

            auto* property = FindMutableProperty(*record, operation.property.c_str());
            if (!property || property->value.GetKind() != PropertyValue::Kind::Array)
                return;

            auto values = property->value.GetArray();
            const auto insertIndex = operation.hasIndex && operation.index < values.size()
                ? operation.index
                : values.size();
            values.insert(values.begin() + static_cast<std::ptrdiff_t>(insertIndex), PropertyValue::OwnedReference(operation.object));
            property->value = PropertyValue::Array(std::move(values));
        }

        static void ApplyRemoveOwned(ObjectGraphDocument& graph, const PatchOperation& operation)
        {
            auto* record = FindMutableRecord(graph, operation.target);
            if (!record)
                return;

            auto* property = FindMutableProperty(*record, operation.property.c_str());
            if (!property || property->value.GetKind() != PropertyValue::Kind::Array)
                return;

            auto values = property->value.GetArray();
            values.erase(std::remove_if(values.begin(), values.end(), [&operation](const PropertyValue& value)
            {
                return value.GetKind() == PropertyValue::Kind::OwnedReference && value.GetObjectId() == operation.object;
            }), values.end());
            property->value = PropertyValue::Array(std::move(values));
        }

        static void ApplyMoveOwned(ObjectGraphDocument& graph, const PatchOperation& operation)
        {
            auto* record = FindMutableRecord(graph, operation.target);
            if (!record)
                return;

            auto* property = FindMutableProperty(*record, operation.property.c_str());
            if (!property || property->value.GetKind() != PropertyValue::Kind::Array)
                return;

            auto values = property->value.GetArray();
            auto found = std::find_if(values.begin(), values.end(), [&operation](const PropertyValue& value)
            {
                return value.GetKind() == PropertyValue::Kind::OwnedReference && value.GetObjectId() == operation.object;
            });

            if (found == values.end())
                return;

            auto moved = *found;
            values.erase(found);
            const auto insertIndex = operation.hasIndex && operation.index < values.size()
                ? operation.index
                : values.size();
            values.insert(values.begin() + static_cast<std::ptrdiff_t>(insertIndex), std::move(moved));
            property->value = PropertyValue::Array(std::move(values));
        }

        static void ApplyRemoveObject(ObjectGraphDocument& graph, const PatchOperation& operation)
        {
            auto* record = FindMutableRecord(graph, operation.target);
            if (!record)
                return;

            std::vector<ObjectId> ownedChildren;
            for (const auto& property : record->properties)
            {
                if (property.value.GetKind() != PropertyValue::Kind::Array)
                    continue;

                for (const auto& value : property.value.GetArray())
                {
                    if (value.GetKind() == PropertyValue::Kind::OwnedReference)
                        ownedChildren.push_back(value.GetObjectId());
                }
            }

            record->state = ObjectRecordState::Removed;
            for (const auto& child : ownedChildren)
            {
                PatchOperation childOperation;
                childOperation.target = child;
                ApplyRemoveObject(graph, childOperation);
            }
        }

        static nlohmann::json ConvertPropertyValue(const PropertyValue& value)
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
                    output.push_back(ConvertPropertyValue(item));
                return output;
            }
            case PropertyValue::Kind::Object:
            {
                nlohmann::json output = nlohmann::json::object();
                for (const auto& property : value.GetObject())
                    output[property.first] = ConvertPropertyValue(property.second);
                return output;
            }
            default:
                return nullptr;
            }
        }

        static bool ContainsAssetReference(const PropertyValue& value)
        {
            switch (value.GetKind())
            {
            case PropertyValue::Kind::ObjectReference:
                return value.GetObjectReference().guid.IsValid();
            case PropertyValue::Kind::Array:
                return std::any_of(
                    value.GetArray().begin(),
                    value.GetArray().end(),
                    [](const PropertyValue& item)
                    {
                        return ContainsAssetReference(item);
                    });
            case PropertyValue::Kind::Object:
                return std::any_of(
                    value.GetObject().begin(),
                    value.GetObject().end(),
                    [](const auto& property)
                    {
                        return ContainsAssetReference(property.second);
                    });
            default:
                return false;
            }
        }

        static void ApplyDeferredAssetReferenceHints(
            Components::Component& component,
            const ObjectRecord& record,
            InstanceContext& context)
        {
            if (RecordTypeMatchesComponent<Components::MeshFilter>(record) &&
                component.GetType() == NLS_TYPEOF(Components::MeshFilter))
            {
                auto* meshFilter = static_cast<Components::MeshFilter*>(&component);
                const auto* mesh = FindProperty(record, "mesh");
                if (mesh && mesh->value.GetKind() == PropertyValue::Kind::ObjectReference &&
                    mesh->value.GetObjectReference().guid.IsValid())
                {
                    const auto& reference = mesh->value.GetObjectReference();
                    const auto path = ResolveAssetReferencePath(reference);
                    meshFilter->SetMeshObjectIdentifier(reference);
                    if (Core::ServiceLocator::Contains<Core::ResourceManagement::MeshManager>())
                    {
                        auto& meshManager = NLS_SERVICE(Core::ResourceManagement::MeshManager);
                        if (auto* cached = FindCachedMeshResource(meshManager, path, context))
                        {
                            meshFilter->SetResolvedMeshFromReference(cached);
                            return;
                        }
                    }
                }
                return;
            }

            if (RecordTypeMatchesComponent<Components::MeshRenderer>(record) &&
                component.GetType() == NLS_TYPEOF(Components::MeshRenderer))
            {
                auto* meshRenderer = static_cast<Components::MeshRenderer*>(&component);
                const auto* materials = FindProperty(record, "materials");
                if (!materials || materials->value.GetKind() != PropertyValue::Kind::Array)
                    return;

                NLS::Array<NLS::Engine::Serialize::ObjectIdentifier> references;
                NLS::Array<std::string> paths;
                for (const auto& value : materials->value.GetArray())
                {
                    if (value.GetKind() == PropertyValue::Kind::ObjectReference && value.GetObjectReference().guid.IsValid())
                    {
                        references.push_back(value.GetObjectReference());
                        paths.push_back(ResolveAssetReferencePath(value.GetObjectReference()));
                    }
                    else
                    {
                        references.push_back({});
                        paths.push_back({});
                    }
                }
                meshRenderer->SetMaterialObjectIdentifiers(references);
                meshRenderer->SetMaterialPathHints(paths);
                if (Core::ServiceLocator::Contains<Core::ResourceManagement::MaterialManager>())
                {
                    for (size_t index = 0; index < paths.size() && index < Components::MeshRenderer::kMaxMaterialCount; ++index)
                    {
                        if (paths[index].empty())
                            continue;

                        if (auto* cached = FindCachedMaterialResource(
                                NLS_SERVICE(Core::ResourceManagement::MaterialManager),
                                paths[index],
                                context))
                        {
                            meshRenderer->SetResolvedMaterialFromReference(static_cast<uint8_t>(index), *cached);
                        }
                    }
                }
            }
        }

        static Core::ResourceManagement::MeshManager::Mesh* FindCachedMeshResource(
            Core::ResourceManagement::MeshManager& meshManager,
            const std::string& path,
            InstanceContext& context)
        {
            const auto candidates = BuildEquivalentResourcePathCandidates(
                path,
                Core::ResourceManagement::MeshManager::ResolveResourcePath(path),
                Core::ResourceManagement::MeshManager::ProjectAssetsRoot());
            return FindCachedResourceByEquivalentPath(
                meshManager,
                candidates,
                context.meshResourcesByNormalizedPath,
                context.meshResourcePathIndexBuilt);
        }

        static Core::ResourceManagement::MaterialManager::Material* FindCachedMaterialResource(
            Core::ResourceManagement::MaterialManager& materialManager,
            const std::string& path,
            InstanceContext& context)
        {
            const auto candidates = BuildEquivalentResourcePathCandidates(
                path,
                Core::ResourceManagement::MaterialManager::ResolveResourcePath(path),
                Core::ResourceManagement::MeshManager::ProjectAssetsRoot());
            return FindCachedResourceByEquivalentPath(
                materialManager,
                candidates,
                context.materialResourcesByNormalizedPath,
                context.materialResourcePathIndexBuilt);
        }

        static std::string ToProjectRelativeResourcePath(
            const std::string& path,
            const std::string& projectAssetsRoot)
        {
            if (path.empty() || projectAssetsRoot.empty())
                return {};

            const auto absolutePath = std::filesystem::path(path).lexically_normal();
            if (!absolutePath.is_absolute())
                return {};

            auto assetsRoot = std::filesystem::path(projectAssetsRoot).lexically_normal();
            while (!assetsRoot.empty() && !assetsRoot.has_filename())
                assetsRoot = assetsRoot.parent_path();

            const auto projectRoot = assetsRoot.parent_path();
            if (projectRoot.empty())
                return {};

            const auto relative = absolutePath.lexically_relative(projectRoot.lexically_normal());
            if (relative.empty() || relative.is_absolute())
                return {};

            for (const auto& part : relative)
            {
                if (part == "..")
                    return {};
            }

            return relative.generic_string();
        }

        static std::vector<std::string> BuildEquivalentResourcePathCandidates(
            const std::string& path,
            const std::string& resolvedPath,
            const std::string& projectAssetsRoot)
        {
            std::vector<std::string> candidates;
            auto addCandidate = [&candidates](const std::string& candidate)
            {
                if (candidate.empty() ||
                    std::find(candidates.begin(), candidates.end(), candidate) != candidates.end())
                {
                    return;
                }
                candidates.push_back(candidate);
            };

            auto addPathVariants = [&addCandidate](const std::string& candidate)
            {
                if (candidate.empty())
                    return;

                addCandidate(candidate);
                const auto normalized = std::filesystem::path(candidate).lexically_normal();
                addCandidate(normalized.string());
                addCandidate(normalized.generic_string());
            };

            addPathVariants(path);
            addPathVariants(resolvedPath);
            addPathVariants(ToProjectRelativeResourcePath(
                resolvedPath.empty() ? path : resolvedPath,
                projectAssetsRoot));
            return candidates;
        }

        static std::string NormalizeResourceCacheLookupKey(std::string path)
        {
            if (path.empty())
                return {};

            std::replace(path.begin(), path.end(), '\\', '/');
            path = std::filesystem::path(path).lexically_normal().generic_string();
            return path;
        }

        template <typename ResourceManagerType>
        using ResourcePointerForManager = decltype(std::declval<ResourceManagerType&>().GetResource(std::declval<std::string>(), false));

        template <typename ResourceManagerType>
        static void BuildNormalizedResourceCacheIndex(
            ResourceManagerType& resourceManager,
            std::unordered_map<std::string, ResourcePointerForManager<ResourceManagerType>>& normalizedResourceCache)
        {
            normalizedResourceCache.clear();
            const auto resources = resourceManager.GetResources();
            normalizedResourceCache.reserve(resources.size());
            for (const auto& [resourcePath, resource] : resources)
            {
                if (resource == nullptr)
                    continue;

                const auto normalizedResourcePath = NormalizeResourceCacheLookupKey(resourcePath);
                if (!normalizedResourcePath.empty())
                    normalizedResourceCache.try_emplace(normalizedResourcePath, resource);
            }
        }

        template <typename ResourceManagerType>
        static ResourcePointerForManager<ResourceManagerType> FindCachedResourceByEquivalentPath(
            ResourceManagerType& resourceManager,
            const std::vector<std::string>& candidates,
            std::unordered_map<std::string, ResourcePointerForManager<ResourceManagerType>>& normalizedResourceCache,
            bool& normalizedResourceCacheBuilt)
        {
            for (const auto& candidate : candidates)
            {
                if (auto* cached = resourceManager.GetResource(candidate, false))
                    return cached;
            }

            std::vector<std::string> normalizedCandidates;
            normalizedCandidates.reserve(candidates.size());
            for (const auto& candidate : candidates)
            {
                auto normalized = NormalizeResourceCacheLookupKey(candidate);
                if (!normalized.empty() &&
                    std::find(normalizedCandidates.begin(), normalizedCandidates.end(), normalized) ==
                        normalizedCandidates.end())
                {
                    normalizedCandidates.push_back(std::move(normalized));
                }
            }

            if (!normalizedResourceCacheBuilt)
            {
                BuildNormalizedResourceCacheIndex(resourceManager, normalizedResourceCache);
                normalizedResourceCacheBuilt = true;
            }

            for (const auto& normalizedCandidate : normalizedCandidates)
            {
                const auto cached = normalizedResourceCache.find(normalizedCandidate);
                if (cached != normalizedResourceCache.end())
                    return cached->second;
            }
            return ResourcePointerForManager<ResourceManagerType> {};
        }

        static void AnalyzeObjectTypes(
            const ObjectGraphDocument& document,
            const LoadPolicy& policy,
            SerializationDiagnosticList& diagnostics)
        {
            for (const auto& object : document.objects)
            {
                if (!IsInstantiableRecordState(object.state))
                    continue;

                const auto type = NLS::meta::Type::GetFromName(object.typeName);
                if (type.IsValid())
                    continue;

                diagnostics.Add({
                    SerializationDiagnosticCode::UnknownType,
                    policy.unknownTypePolicy == UnknownTypePolicy::Fail
                        ? SerializationDiagnosticSeverity::Error
                        : SerializationDiagnosticSeverity::Warning,
                    "Object graph contains an unknown object type: " + object.typeName
                });
            }
        }

        static void AnalyzeReflectedObjectReferenceShapes(
            const ObjectGraphDocument& document,
            const LoadPolicy& policy,
            SerializationDiagnosticList& diagnostics)
        {
            for (const auto& object : document.objects)
            {
                if (!IsInstantiableRecordState(object.state))
                    continue;

                const auto type = NLS::meta::Type::GetFromName(object.typeName);
                if (!type.IsValid())
                    continue;

                for (const auto& property : object.properties)
                {
                    const auto field = type.GetField(property.name);
                    if (!field.IsValid())
                        continue;

                    if (policy.deferAssetReferenceResolution &&
                        IsDeferredAssetProperty(object, property.name))
                    {
                        continue;
                    }

                    const auto fieldType = field.GetType();
                    if (fieldType.IsArray() && Internal::IsPPtrTypeName(fieldType.GetArrayType()))
                    {
                        if (!IsPPtrArrayPropertyValue(property.value))
                            AddInvalidObjectReferenceDiagnostic(policy, diagnostics, object, property);
                        continue;
                    }

                    if (Internal::IsPPtrTypeName(fieldType) && !IsPPtrPropertyValue(property.value))
                        AddInvalidObjectReferenceDiagnostic(policy, diagnostics, object, property);
                }
            }
        }

        static bool IsPPtrPropertyValue(const PropertyValue& value)
        {
            return value.GetKind() == PropertyValue::Kind::ObjectReference;
        }

        static bool IsPPtrArrayPropertyValue(const PropertyValue& value)
        {
            if (value.GetKind() != PropertyValue::Kind::Array)
                return false;

            for (const auto& item : value.GetArray())
            {
                if (!IsPPtrPropertyValue(item))
                    return false;
            }
            return true;
        }

        static void AddInvalidObjectReferenceDiagnostic(
            const LoadPolicy& policy,
            SerializationDiagnosticList& diagnostics,
            const ObjectRecord& object,
            const PropertyRecord& property)
        {
            diagnostics.Add({
                SerializationDiagnosticCode::InvalidPropertyType,
                policy.invalidReferencePolicy == InvalidReferencePolicy::Fail
                    ? SerializationDiagnosticSeverity::Error
                    : SerializationDiagnosticSeverity::Warning,
                "Object graph property \"" + object.typeName + "." + property.name +
                    "\" must use a Unity-style object reference shape."
            });
        }

        static void AnalyzeAssetReferences(
            const ObjectGraphDocument& document,
            const LoadPolicy& policy,
            SerializationDiagnosticList& diagnostics)
        {
            if (document.basePrefab.has_value())
                AnalyzeObjectIdentifier(PropertyValue::ObjectReference(*document.basePrefab), policy, diagnostics);

            for (const auto& object : document.objects)
            {
                if (!IsInstantiableRecordState(object.state))
                    continue;

                for (const auto& property : object.properties)
                    AnalyzeObjectIdentifier(property.value, policy, diagnostics);
            }
        }

        static void AnalyzeObjectIdentifier(
            const PropertyValue& value,
            const LoadPolicy& policy,
            SerializationDiagnosticList& diagnostics)
        {
            switch (value.GetKind())
            {
            case PropertyValue::Kind::ObjectReference:
            {
                const auto& reference = value.GetObjectReference();
                if (reference.IsAsset() &&
                    Core::ServiceLocator::Contains<Assets::RuntimeAssetDatabase>() &&
                    ResolveRuntimeAssetReferenceEntry(reference) == nullptr)
                {
                    AddMissingAssetDiagnostic(policy, diagnostics);
                    break;
                }

                if (!reference.guid.IsValid() &&
                    (reference.fileType != FileType::NonAssetType || !reference.filePath.empty()))
                {
                    AddMissingAssetDiagnostic(policy, diagnostics);
                }
                break;
            }
            case PropertyValue::Kind::Array:
                for (const auto& item : value.GetArray())
                    AnalyzeObjectIdentifier(item, policy, diagnostics);
                break;
            case PropertyValue::Kind::Object:
                for (const auto& property : value.GetObject())
                    AnalyzeObjectIdentifier(property.second, policy, diagnostics);
                break;
            default:
                break;
            }
        }

        static void AddMissingAssetDiagnostic(
            const LoadPolicy& policy,
            SerializationDiagnosticList& diagnostics)
        {
            diagnostics.Add({
                SerializationDiagnosticCode::MissingAsset,
                policy.missingAssetPolicy == MissingAssetPolicy::Fail
                    ? SerializationDiagnosticSeverity::Error
                    : SerializationDiagnosticSeverity::Warning,
                "Object graph contains a missing or invalid asset reference."
            });
        }
    };
}
