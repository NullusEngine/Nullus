#pragma once

#include <string>
#include <unordered_map>
#include <functional>
#include <optional>

#include "Components/Component.h"
#include "GameObject.h"
#include "Reflection/Field.h"
#include "Rendering/ExternalReflection.h"
#include "Rendering/Resources/Material.h"
#include "SceneSystem/Scene.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/ObjectGraphPPtr.h"
#include "Serialize/PPtr.h"
#include "Serialize/PrefabDocument.h"

namespace NLS::Engine::Serialize
{
    class ObjectGraphSerializer
    {
    public:
        using SceneReferenceMapper = std::function<std::optional<ObjectIdentifier>(InstanceID)>;
        enum class GameObjectOwnershipMode
        {
            SceneFlatList,
            OwnedHierarchy
        };

        static ObjectGraphDocument SerializeScene(const SceneSystem::Scene& scene)
        {
            SerializationContext context;

            ObjectGraphDocument document;
            document.format = "Nullus.ObjectGraph.Scene";
            document.version = 2;
            document.documentId = NLS::Guid::NewDeterministic("ObjectGraph.Scene.Document");

            const auto sceneId = context.GetId(&scene, "Scene.Root");
            document.root = sceneId;

            ObjectRecord sceneRecord;
            sceneRecord.id = sceneId;
            sceneRecord.localIdentifierInFile = MakeLocalIdentifierInFile(sceneId);
            sceneRecord.typeName = NLS_TYPEOF(SceneSystem::Scene).GetName();
            sceneRecord.debugName = "Scene";

            size_t gameObjectIndex = 0;
            for (auto* gameObject : scene.GetGameObjects())
            {
                if (!gameObject || gameObject->IsEditorTransient())
                    continue;

                context.GetId(gameObject, MakeGameObjectLabel(*gameObject, gameObjectIndex++));
            }
            for(auto* gameObject:scene.GetGameObjects())
                if(gameObject&&!gameObject->IsEditorTransient())PreassignComponentIds(*gameObject,context);

            PropertyValue::ArrayValue gameObjects;
            for (auto* gameObject : scene.GetGameObjects())
            {
                if (!gameObject || gameObject->IsEditorTransient())
                    continue;

                const auto objectId = context.GetId(gameObject, "");
                gameObjects.push_back(PropertyValue::OwnedReference(objectId));
                WriteGameObject(*gameObject, objectId, context, document, GameObjectOwnershipMode::SceneFlatList);
            }

            sceneRecord.properties.push_back({"gameObjects", PropertyValue::Array(std::move(gameObjects))});
            document.objects.push_back(std::move(sceneRecord));
            return document;
        }

        static PrefabDocument SerializePrefab(const GameObject& root)
        {
            SerializationContext context;

            PrefabDocument prefab;
            prefab.graph.format = "Nullus.ObjectGraph.Prefab";
            prefab.graph.version = 2;
            prefab.graph.documentId = NLS::Guid::NewDeterministic("ObjectGraph.Prefab.Document:" + root.GetName());

            PreassignGameObjectIds(root, context);
            const auto rootId = context.GetId(&root, "");
            prefab.graph.root = rootId;
            WriteGameObjectRecursive(root, rootId, context, prefab.graph, GameObjectOwnershipMode::OwnedHierarchy);
            return prefab;
        }

        static ObjectRecord SerializeObjectRecord(const NLS::Object& object, ObjectId objectId,const SceneReferenceMapper& mapper = {})
        {
            const auto type = object.GetType();
            ObjectRecord record;
            record.id = objectId;
            record.localIdentifierInFile = MakeLocalIdentifierInFile(objectId);
            record.typeName = type.IsValid() ? type.GetName() : object.GetObjectTypeName();
            record.debugName = record.typeName;
            if (const auto* component = dynamic_cast<const Components::Component*>(&object);
                component == nullptr || !component->SerializeObjectGraphProperties(record.properties))
            {
                if (type.IsValid())
                    WriteReflectedFields(object, type, record,mapper);
            }

            // `enabled` is deliberately graph-owned rather than reflected: it
            // belongs to every component, including components that serialize
            // their own payload (such as ScriptComponent).  Older v1 graphs do
            // not carry it and therefore retain Component's default `true` on
            // load.
            if (const auto* component = dynamic_cast<const Components::Component*>(&object))
                record.properties.push_back({"enabled", PropertyValue::Bool(component->IsSelfEnabled())});
            return record;
        }

    private:
        class SerializationContext
        {
        public:
            ObjectId GetId(const void* object, const std::string& label)
            {
                if (const auto found = m_ids.find(object); found != m_ids.end())
                    return found->second;

                auto id = ObjectId(NLS::Guid::NewDeterministic(label));
                m_ids.emplace(object, id);
                return id;
            }
            std::optional<ObjectId> FindId(const void* object)const
            {const auto found=m_ids.find(object);return found==m_ids.end()?std::nullopt:std::optional<ObjectId>(found->second);}

        private:
            std::unordered_map<const void*, ObjectId> m_ids;
        };

        static std::string MakeGameObjectLabel(const GameObject& gameObject, size_t index)
        {
            return "GameObject:" + std::to_string(index) + ":" + gameObject.GetName() + ":" + gameObject.GetTag();
        }

        static std::string MakeComponentLabel(const ObjectId& ownerId, const Components::Component& component, size_t index)
        {
            const auto type = component.GetType();
            return "Component:" + ownerId.GetGuid().ToString() + ":" + std::to_string(index) + ":" +
                   (type.IsValid() ? type.GetName() : component.GetObjectTypeName());
        }

        static void WriteGameObject(
            const GameObject& gameObject,
            ObjectId objectId,
            SerializationContext& context,
            ObjectGraphDocument& document,
            GameObjectOwnershipMode ownershipMode)
        {
            auto record = SerializeObjectRecord(gameObject, objectId);
            record.debugName = gameObject.GetName();
            record.debugPath = "/" + gameObject.GetName();

            if (auto* parent = gameObject.GetParent())
            {
                record.properties.push_back({
                    "parent",
                    PropertyValue::ObjectReference(
                        ObjectIdentifier::LocalObject(MakeLocalIdentifierInFile(context.GetId(parent, ""))))
                });
            }
            else
                record.properties.push_back({"parent", PropertyValue::Null()});

            PropertyValue::ArrayValue components;
            const auto& sourceComponents = gameObject.GetComponents();
            for (size_t index = 0; index < sourceComponents.size(); ++index)
            {
                auto* component = sourceComponents[index].get();
                if (!component)
                    continue;

                const auto componentId = context.GetId(component, MakeComponentLabel(objectId, *component, index));
                components.push_back(PropertyValue::OwnedReference(componentId));
                WriteComponent(*component, componentId, document,context);
            }
            record.properties.push_back({"components", PropertyValue::Array(std::move(components))});

            if (ownershipMode == GameObjectOwnershipMode::OwnedHierarchy)
            {
                PropertyValue::ArrayValue children;
                for (auto* child : gameObject.GetChildren())
                {
                    if (child)
                        children.push_back(PropertyValue::OwnedReference(context.GetId(child, "")));
                }
                record.properties.push_back({"children", PropertyValue::Array(std::move(children))});
            }

            document.objects.push_back(std::move(record));
        }

        static void PreassignGameObjectIds(const GameObject& root, SerializationContext& context)
        {
            size_t index = 0;
            PreassignGameObjectIdsRecursive(root, context, index);
        }

        static void PreassignGameObjectIdsRecursive(const GameObject& gameObject, SerializationContext& context, size_t& index)
        {
            context.GetId(&gameObject, MakeGameObjectLabel(gameObject, index++));
            PreassignComponentIds(gameObject,context);
            for (auto* child : gameObject.GetChildren())
            {
                if (child)
                    PreassignGameObjectIdsRecursive(*child, context, index);
            }
        }

        static void WriteGameObjectRecursive(
            const GameObject& gameObject,
            ObjectId objectId,
            SerializationContext& context,
            ObjectGraphDocument& document,
            GameObjectOwnershipMode ownershipMode)
        {
            WriteGameObject(gameObject, objectId, context, document, ownershipMode);
            for (auto* child : gameObject.GetChildren())
            {
                if (!child)
                    continue;

                WriteGameObjectRecursive(*child, context.GetId(child, ""), context, document, ownershipMode);
            }
        }

        static void PreassignComponentIds(const GameObject& object,SerializationContext& context)
        {
            const auto owner=context.GetId(&object,"");const auto& components=object.GetComponents();
            for(size_t i=0;i<components.size();++i)if(components[i])context.GetId(components[i].get(),MakeComponentLabel(owner,*components[i],i));
        }

        static void WriteComponent(const Components::Component& component, ObjectId componentId, ObjectGraphDocument& document,const SerializationContext& context)
        {
            auto record = SerializeObjectRecord(component, componentId,[&context](InstanceID instance)->std::optional<ObjectIdentifier>
            {
                const auto id=context.FindId(NLS::Object::IDToPointer(instance));
                return id?std::optional<ObjectIdentifier>(ObjectIdentifier::LocalObject(MakeLocalIdentifierInFile(*id))):std::nullopt;
            });
            const auto type = component.GetType();
            record.debugName = type.IsValid() ? type.GetName() : component.GetObjectTypeName();
            document.objects.push_back(std::move(record));
        }

        static void WriteReflectedFields(const NLS::Object& object, const NLS::meta::Type& type, ObjectRecord& record,const SceneReferenceMapper& mapper)
        {
            auto instance = NLS::meta::Variant(
                const_cast<NLS::Object*>(&object),
                NLS::meta::variant_policy::WrapObject {});
            for (const auto& field : type.GetFields())
            {
                const auto value = field.GetValue(instance);
                record.properties.push_back({field.GetName(), ConvertVariantValue(value,mapper)});
            }
        }

        static PropertyValue ConvertVariantValue(const NLS::meta::Variant& value,const SceneReferenceMapper& mapper)
        {
            const auto valueType = value.GetType();
            if(mapper&&(valueType==NLS_TYPEOF(PPtr<GameObject>)||valueType==NLS_TYPEOF(PPtr<Components::Component>)))
            {
                const auto instance=valueType==NLS_TYPEOF(PPtr<GameObject>)?value.GetValue<PPtr<GameObject>>().GetInstanceID():value.GetValue<PPtr<Components::Component>>().GetInstanceID();
                if(const auto reference=mapper(instance))return PropertyValue::ObjectReference(*reference);
            }
            if (valueType.IsArray())
            {
                if (Internal::IsPPtrTypeName(valueType.GetArrayType()))
                {
                    PropertyValue::ArrayValue values;
                    const auto array = value.GetArray();
                    values.reserve(array.Size());
                    for (size_t index = 0; index < array.Size(); ++index)
                        values.push_back(ConvertVariantValue(array.GetValue(index),mapper));
                    return PropertyValue::Array(std::move(values));
                }

                PropertyValue::ArrayValue values;
                const auto array = value.GetArray();
                values.reserve(array.Size());
                for (size_t index = 0; index < array.Size(); ++index)
                    values.push_back(ConvertVariantValue(array.GetValue(index),mapper));
                return PropertyValue::Array(std::move(values));
            }
            if (Internal::IsPPtrTypeName(valueType))
                return Internal::SerializePPtrValueOrThrow(value);

            return ConvertJsonValue(value.SerializeJson().native());
        }

        static PropertyValue ConvertJsonValue(const nlohmann::json& json)
        {
            if (json.is_null())
                return PropertyValue::Null();
            if (json.is_boolean())
                return PropertyValue::Bool(json.get<bool>());
            if (json.is_number_integer())
                return PropertyValue::Integer(json.get<int64_t>());
            if (json.is_number_float())
                return PropertyValue::Number(json.get<double>());
            if (json.is_string())
                return PropertyValue::String(json.get<std::string>());
            if (json.is_array())
            {
                PropertyValue::ArrayValue values;
                for (const auto& item : json)
                    values.push_back(ConvertJsonValue(item));
                return PropertyValue::Array(std::move(values));
            }
            if (json.is_object())
            {
                PropertyValue::ObjectValue values;
                for (const auto& item : json.items())
                    values.push_back({item.key(), ConvertJsonValue(item.value())});
                return PropertyValue::Object(std::move(values));
            }

            return PropertyValue::Null();
        }
    };
}
