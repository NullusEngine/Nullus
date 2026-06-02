#pragma once

#include <algorithm>
#include <stdexcept>
#include <string>

#include <Json/json.hpp>

#include "Serialize/ObjectGraphDocument.h"

namespace NLS::Engine::Serialize
{
    class ObjectGraphWriter
    {
    public:
        static std::string Write(const ObjectGraphDocument& document)
        {
            nlohmann::json root;
            root["format"] = document.format;
            root["version"] = document.version;
            root["documentId"] = document.documentId.ToString();
            root["root"] = document.root.GetGuid().ToString();
            if (document.basePrefab.has_value())
                root["basePrefab"] = WriteObjectReference(*document.basePrefab);

            auto objects = document.objects;
            std::sort(objects.begin(), objects.end(), [](const ObjectRecord& lhs, const ObjectRecord& rhs)
            {
                return lhs.id < rhs.id;
            });

            root["objects"] = nlohmann::json::array();
            for (const auto& object : objects)
                root["objects"].push_back(WriteObject(object));

            if (!document.overrides.empty())
            {
                root["overrides"] = nlohmann::json::array();
                for (const auto& operation : document.overrides)
                    root["overrides"].push_back(WritePatchOperation(operation));
            }

            if (!document.prefabInstances.empty())
            {
                root["prefabInstances"] = nlohmann::json::array();
                for (const auto& prefabInstance : document.prefabInstances)
                    root["prefabInstances"].push_back(WritePrefabInstance(prefabInstance));
            }

            return root.dump(4) + "\n";
        }

    private:
        static nlohmann::json WriteObject(const ObjectRecord& object)
        {
            nlohmann::json json;
            json["id"] = object.id.GetGuid().ToString();
            json["fileID"] = object.localIdentifierInFile;
            json["type"] = object.typeName;
            if (!object.debugName.empty())
                json["debugName"] = object.debugName;
            if (!object.debugPath.empty())
                json["debugPath"] = object.debugPath;
            switch (object.state)
            {
            case ObjectRecordState::Alive:
                json["state"] = "Alive";
                break;
            case ObjectRecordState::Stripped:
                json["state"] = "Stripped";
                break;
            case ObjectRecordState::Removed:
            default:
                json["state"] = "Removed";
                break;
            }

            nlohmann::json properties = nlohmann::json::object();
            auto propertyRecords = object.properties;
            std::sort(propertyRecords.begin(), propertyRecords.end(), [](const PropertyRecord& lhs, const PropertyRecord& rhs)
            {
                return lhs.name < rhs.name;
            });
            for (const auto& property : propertyRecords)
                properties[property.name] = WriteValue(property.value);
            json["properties"] = std::move(properties);
            return json;
        }

        static nlohmann::json WriteValue(const PropertyValue& value)
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
            case PropertyValue::Kind::OwnedReference:
                return nlohmann::json{{"$owned", value.GetObjectId().GetGuid().ToString()}};
            case PropertyValue::Kind::ObjectReference:
                return WriteObjectReference(value.GetObjectReference());
            case PropertyValue::Kind::Array:
            {
                nlohmann::json output = nlohmann::json::array();
                for (const auto& item : value.GetArray())
                    output.push_back(WriteValue(item));
                return output;
            }
            case PropertyValue::Kind::Object:
            {
                nlohmann::json output = nlohmann::json::object();
                for (const auto& property : value.GetObject())
                    output[property.first] = WriteValue(property.second);
                return output;
            }
            default:
                return nullptr;
            }
        }

        static nlohmann::json WriteObjectReference(const ObjectIdentifier& reference)
        {
            if (!IsSerializableObjectReference(reference))
                throw std::invalid_argument("ObjectIdentifier must be empty or a complete Unity-style object reference.");

            nlohmann::json output;
            output["fileID"] = reference.localIdentifierInFile;
            output["guid"] = reference.guid.IsValid() ? reference.guid.ToString() : std::string {};
            output["type"] = static_cast<int32_t>(reference.fileType);
            if (!reference.filePath.empty())
                output["filePath"] = reference.filePath;
            return output;
        }

        static bool IsSerializableObjectReference(const ObjectIdentifier& reference)
        {
            return reference.IsValid() ||
                (reference.localIdentifierInFile == 0 &&
                    !reference.guid.IsValid() &&
                    reference.fileType == FileType::NonAssetType &&
                    reference.filePath.empty());
        }

        static nlohmann::json WritePatchOperation(const PatchOperation& operation)
        {
            nlohmann::json json;
            switch (operation.type)
            {
            case PatchOperationType::ReplaceProperty:
                json["op"] = "replaceProperty";
                json["target"] = operation.target.GetGuid().ToString();
                json["property"] = operation.property;
                json["value"] = WriteValue(operation.value);
                break;
            case PatchOperationType::InsertOwned:
                json["op"] = "insertOwned";
                json["owner"] = operation.target.GetGuid().ToString();
                json["property"] = operation.property;
                json["object"] = operation.object.GetGuid().ToString();
                if (operation.hasIndex)
                    json["index"] = operation.index;
                break;
            case PatchOperationType::RemoveOwned:
                json["op"] = "removeOwned";
                json["owner"] = operation.target.GetGuid().ToString();
                json["property"] = operation.property;
                json["object"] = operation.object.GetGuid().ToString();
                break;
            case PatchOperationType::MoveOwned:
                json["op"] = "moveOwned";
                json["owner"] = operation.target.GetGuid().ToString();
                json["property"] = operation.property;
                json["object"] = operation.object.GetGuid().ToString();
                if (operation.hasIndex)
                    json["index"] = operation.index;
                break;
            case PatchOperationType::RemoveObject:
                json["op"] = "removeObject";
                json["target"] = operation.target.GetGuid().ToString();
                break;
            default:
                throw std::invalid_argument("Unsupported patch operation type.");
            }
            return json;
        }

        static nlohmann::json WritePrefabInstance(const PrefabInstanceRecord& prefabInstance)
        {
            nlohmann::json json;
            json["instanceRoot"] = prefabInstance.instanceRoot.GetGuid().ToString();
            json["sourcePrefab"] = WriteObjectReference(prefabInstance.sourcePrefab);
            json["generatedReadOnly"] = prefabInstance.generatedReadOnly;

            json["modifications"] = nlohmann::json::array();
            for (const auto& modification : prefabInstance.modifications)
                json["modifications"].push_back(WritePatchOperation(modification));

            json["addedObjects"] = nlohmann::json::array();
            for (const auto& addedObject : prefabInstance.addedObjects)
                json["addedObjects"].push_back(WriteObject(addedObject));

            json["correspondence"] = nlohmann::json::array();
            for (const auto& correspondence : prefabInstance.correspondence)
            {
                json["correspondence"].push_back({
                    {"sourceObject", correspondence.sourceObject.GetGuid().ToString()},
                    {"instanceObject", correspondence.instanceObject.GetGuid().ToString()}
                });
            }

            return json;
        }
    };
}
