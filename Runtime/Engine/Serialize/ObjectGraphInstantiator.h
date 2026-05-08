#pragma once

#include <memory>
#include <optional>
#include <algorithm>
#include <unordered_map>

#include "Components/Component.h"
#include "Components/TransformComponent.h"
#include "GameObject.h"
#include "Reflection/Field.h"
#include "Reflection/TypeCreator.h"
#include "SceneSystem/Scene.h"
#include "Serialize/ObjectGraphDocument.h"
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

    class ObjectGraphInstantiator
    {
    public:
        static DocumentAnalysisResult AnalyzeDocument(const ObjectGraphDocument& document, const LoadPolicy& policy)
        {
            DocumentAnalysisResult result;
            result.diagnostics = document.Validate();

            AnalyzeObjectTypes(document, policy, result.diagnostics);
            AnalyzeAssetReferences(document, policy, result.diagnostics);
            return result;
        }

        static std::unique_ptr<SceneSystem::Scene> InstantiateScene(const ObjectGraphDocument& document)
        {
            return InstantiateSceneStrict(document);
        }

        static SceneInstantiationResult InstantiateScene(const ObjectGraphDocument& document, const LoadPolicy& policy)
        {
            SceneInstantiationResult result;
            result.diagnostics = AnalyzeDocument(document, policy).diagnostics;
            if (result.diagnostics.HasErrors())
                return result;

            result.scene = InstantiateSceneStrict(document);
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
            if (document.Validate().HasErrors())
                return nullptr;

            const auto* sceneRecord = FindRecord(document, document.root);
            if (!sceneRecord || sceneRecord->typeName != "NLS::Engine::SceneSystem::Scene")
                return nullptr;

            auto scene = std::make_unique<SceneSystem::Scene>();
            InstanceContext context;

            const auto* gameObjects = FindProperty(*sceneRecord, "gameObjects");
            if (!gameObjects || gameObjects->value.GetKind() != PropertyValue::Kind::Array)
                return scene;

            for (const auto& reference : gameObjects->value.GetArray())
            {
                if (reference.GetKind() != PropertyValue::Kind::OwnedReference)
                    continue;

                const auto* record = FindRecord(document, reference.GetObjectId());
                if (!record || record->typeName != "NLS::Engine::GameObject")
                    continue;

                auto* gameObject = CreateGameObject(*record);
                if (!gameObject)
                    continue;

                context.gameObjects.emplace(record->id, gameObject);
                scene->AddGameObject(gameObject);
            }

            for (const auto& object : document.objects)
            {
                if (object.typeName != "NLS::Engine::GameObject")
                    continue;

                auto* gameObject = FindGameObject(context, object.id);
                if (!gameObject)
                    continue;

                ApplyGameObjectState(*gameObject, object);
                InstantiateComponents(document, object, *gameObject, context);
            }

            for (const auto& object : document.objects)
            {
                if (object.typeName == "NLS::Engine::GameObject")
                    ResolveParent(context, object);
            }

            scene->RebuildRuntimeCachesAfterLoad();
            return scene;
        }

        static PrefabInstantiationResult InstantiatePrefab(const PrefabDocument& prefab, SceneSystem::Scene& scene)
        {
            auto graph = prefab.graph;
            ApplyOverrides(graph);

            PrefabInstantiationResult result;
            InstanceContext context;

            for (const auto& object : graph.objects)
            {
                if (object.typeName != "NLS::Engine::GameObject")
                    continue;

                auto* gameObject = CreateGameObject(object);
                if (!gameObject)
                    continue;

                const auto instanceId = ObjectId(NLS::Guid::NewDeterministic("Prefab.Instance:" + object.id.GetGuid().ToString()));
                result.sourceToInstance.emplace(object.id, instanceId);
                context.gameObjects.emplace(object.id, gameObject);
            }

            for (const auto& object : graph.objects)
            {
                if (object.typeName != "NLS::Engine::GameObject")
                    continue;

                auto* gameObject = FindGameObject(context, object.id);
                if (!gameObject)
                    continue;

                ApplyGameObjectState(*gameObject, object);
                InstantiateComponents(graph, object, *gameObject, context);
                RegisterComponentMappings(object, result, graph);
            }

            for (const auto& object : graph.objects)
            {
                if (object.typeName == "NLS::Engine::GameObject")
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
            std::unordered_map<ObjectId, GameObject*> gameObjects;
            std::unordered_map<ObjectId, Components::Component*> components;
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
            InstanceContext& context)
        {
            const auto* components = FindProperty(gameObjectRecord, "components");
            if (!components || components->value.GetKind() != PropertyValue::Kind::Array)
                return;

            for (size_t index = 0; index < components->value.GetArray().size(); ++index)
            {
                const auto& reference = components->value.GetArray()[index];
                if (reference.GetKind() != PropertyValue::Kind::OwnedReference)
                    continue;

                const auto* componentRecord = FindRecord(document, reference.GetObjectId());
                if (!componentRecord)
                    continue;

                auto* component = EnsureComponent(gameObject, *componentRecord, index);
                if (!component)
                    continue;

                context.components.emplace(componentRecord->id, component);
                ApplyComponentState(*component, *componentRecord);
                gameObject.MoveComponent(component, index);
            }
        }

        static Components::Component* EnsureComponent(GameObject& gameObject, const ObjectRecord& record, size_t index)
        {
            if (record.typeName == "NLS::Engine::Components::TransformComponent")
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

        static void ApplyComponentState(Components::Component& component, const ObjectRecord& record)
        {
            ApplyReflectedFields(component, record, IsNoGraphProperty);
        }

        static void ApplyReflectedFields(
            NLS::meta::Object& object,
            const ObjectRecord& record,
            bool (*isGraphProperty)(const std::string&))
        {
            auto instance = NLS::meta::Variant(&object, NLS::meta::variant_policy::WrapObject {});
            const auto type = object.GetType();

            for (const auto& property : record.properties)
            {
                if (isGraphProperty && isGraphProperty(property.name))
                    continue;

                const auto field = type.GetField(property.name);
                if (!field.IsValid() || field.IsReadOnly())
                    continue;

                const auto json = ConvertPropertyValue(property.value);
                auto value = field.GetType().DeserializeJson(NLS::Json(json));
                field.SetValue(instance, value);
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

            auto* parent = FindGameObject(context, parentProperty->value.GetObjectId());
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
                if (reference.GetKind() != PropertyValue::Kind::OwnedReference)
                    continue;

                const auto* componentRecord = FindRecord(graph, reference.GetObjectId());
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

        static void AnalyzeObjectTypes(
            const ObjectGraphDocument& document,
            const LoadPolicy& policy,
            SerializationDiagnosticList& diagnostics)
        {
            for (const auto& object : document.objects)
            {
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

        static void AnalyzeAssetReferences(
            const ObjectGraphDocument& document,
            const LoadPolicy& policy,
            SerializationDiagnosticList& diagnostics)
        {
            if (document.basePrefab.has_value() && !document.basePrefab->asset.IsValid())
            {
                AddMissingAssetDiagnostic(policy, diagnostics);
            }

            for (const auto& object : document.objects)
            {
                for (const auto& property : object.properties)
                    AnalyzeAssetReferenceValue(property.value, policy, diagnostics);
            }
        }

        static void AnalyzeAssetReferenceValue(
            const PropertyValue& value,
            const LoadPolicy& policy,
            SerializationDiagnosticList& diagnostics)
        {
            switch (value.GetKind())
            {
            case PropertyValue::Kind::AssetReference:
                if (!value.GetAssetReference().asset.IsValid())
                    AddMissingAssetDiagnostic(policy, diagnostics);
                break;
            case PropertyValue::Kind::Array:
                for (const auto& item : value.GetArray())
                    AnalyzeAssetReferenceValue(item, policy, diagnostics);
                break;
            case PropertyValue::Kind::Object:
                for (const auto& property : value.GetObject())
                    AnalyzeAssetReferenceValue(property.second, policy, diagnostics);
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
