#pragma once

#include <optional>
#include <string>

#include <Json/json.hpp>

#include "Serialize/ObjectGraphDocument.h"

namespace NLS::Engine::Serialize
{
    class ObjectGraphReader
    {
    public:
        static std::optional<ObjectGraphDocument> Read(const std::string& text)
        {
            const auto root = nlohmann::json::parse(text, nullptr, false);
            if (root.is_discarded() || !root.is_object())
                return std::nullopt;

            ObjectGraphDocument document;
            document.format = root.value("format", std::string {});
            document.version = root.value("version", 0);

            auto documentId = NLS::Guid::TryParse(root.value("documentId", std::string {}));
            auto rootId = NLS::Guid::TryParse(root.value("root", std::string {}));
            if (!documentId.has_value() || !rootId.has_value())
                return std::nullopt;

            document.documentId = *documentId;
            document.root = ObjectId(*rootId);

            if (const auto basePrefab = root.find("basePrefab"); basePrefab != root.end())
            {
                auto value = ReadValue(*basePrefab);
                if (!value.has_value() || value->GetKind() != PropertyValue::Kind::ObjectReference)
                    return std::nullopt;
                const auto& reference = value->GetObjectReference();
                if (!reference.guid.IsValid())
                    return std::nullopt;
                document.basePrefab = reference;
            }

            const auto objects = root.find("objects");
            if (objects == root.end() || !objects->is_array())
                return std::nullopt;

            for (const auto& objectJson : *objects)
            {
                auto object = ReadObject(objectJson);
                if (!object.has_value())
                    return std::nullopt;
                document.objects.push_back(std::move(*object));
            }

            if (const auto overrides = root.find("overrides"); overrides != root.end())
            {
                if (!overrides->is_array())
                    return std::nullopt;

                for (const auto& operationJson : *overrides)
                {
                    auto operation = ReadPatchOperation(operationJson);
                    if (!operation.has_value())
                        return std::nullopt;
                    document.overrides.push_back(std::move(*operation));
                }
            }

            return document;
        }

    private:
        static std::optional<ObjectRecord> ReadObject(const nlohmann::json& json)
        {
            if (!json.is_object())
                return std::nullopt;

            auto id = NLS::Guid::TryParse(json.value("id", std::string {}));
            if (!id.has_value())
                return std::nullopt;

            const auto fileID = json.find("fileID");
            if (fileID == json.end() || !fileID->is_number_integer())
                return std::nullopt;

            ObjectRecord record;
            record.id = ObjectId(*id);
            record.localIdentifierInFile = fileID->get<int64_t>();
            record.typeName = json.value("type", std::string {});
            record.debugName = json.value("debugName", std::string {});
            record.debugPath = json.value("debugPath", std::string {});
            record.state = json.value("state", std::string {"Alive"}) == "Removed"
                ? ObjectRecordState::Removed
                : ObjectRecordState::Alive;

            const auto properties = json.find("properties");
            if (properties != json.end())
            {
                if (!properties->is_object())
                    return std::nullopt;

                for (const auto& item : properties->items())
                {
                    auto value = ReadValue(item.value());
                    if (!value.has_value())
                        return std::nullopt;
                    record.properties.push_back({item.key(), std::move(*value)});
                }
            }

            return record;
        }

        static std::optional<PropertyValue> ReadValue(const nlohmann::json& json)
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
                {
                    auto value = ReadValue(item);
                    if (!value.has_value())
                        return std::nullopt;
                    values.push_back(std::move(*value));
                }
                return PropertyValue::Array(std::move(values));
            }

            if (json.is_object())
            {
                if (ContainsLegacyOrIncompleteObjectReferenceShape(json))
                    return std::nullopt;

                if (auto found = json.find("$owned"); found != json.end() && found->is_string())
                {
                    auto id = NLS::Guid::TryParse(found->get<std::string>());
                    return id.has_value() ? std::optional<PropertyValue>(PropertyValue::OwnedReference(ObjectId(*id))) : std::nullopt;
                }

                if (auto found = json.find("fileID"); found != json.end() && found->is_number_integer())
                {
                    const auto type = json.find("type");
                    const auto guidNode = json.find("guid");
                    if (type == json.end() || !type->is_number_integer() || guidNode == json.end() || !guidNode->is_string())
                        return std::nullopt;

                    auto referenceGuid = NLS::Guid::Empty();
                    const auto guidText = json.value("guid", std::string {});
                    if (!guidText.empty())
                    {
                        auto parsedGuid = NLS::Guid::TryParse(guidText);
                        if (!parsedGuid.has_value())
                            return std::nullopt;
                        referenceGuid = *parsedGuid;
                    }

                    ObjectIdentifier reference;
                    reference.guid = referenceGuid;
                    reference.localIdentifierInFile = found->get<int64_t>();
                    reference.fileType = static_cast<FileType>(type->get<int32_t>());
                    if (!IsKnownFileType(reference.fileType))
                        return std::nullopt;
                    reference.filePath = json.value("filePath", std::string {});

                    if (reference.guid.IsValid())
                    {
                        if (reference.localIdentifierInFile == 0 ||
                            reference.fileType == FileType::NonAssetType)
                        {
                            return std::nullopt;
                        }
                    }
                    else if (reference.localIdentifierInFile == 0 &&
                        (reference.fileType != FileType::NonAssetType || !reference.filePath.empty()))
                    {
                        return std::nullopt;
                    }
                    else if (!reference.guid.IsValid() && !reference.filePath.empty())
                    {
                        return std::nullopt;
                    }

                    return PropertyValue::ObjectReference(std::move(reference));
                }

                PropertyValue::ObjectValue values;
                for (const auto& item : json.items())
                {
                    auto value = ReadValue(item.value());
                    if (!value.has_value())
                        return std::nullopt;
                    values.push_back({item.key(), std::move(*value)});
                }
                return PropertyValue::Object(std::move(values));
            }

            return PropertyValue::Null();
        }

        static bool ContainsLegacyOrIncompleteObjectReferenceShape(const nlohmann::json& json)
        {
            const bool hasLegacyReference = json.contains("$ref") || json.contains("$asset");
            const bool hasFileID = json.contains("fileID");
            const bool hasGuid = json.contains("guid");
            const bool hasFilePath = json.contains("filePath");
            const bool hasObjectReferenceKey = hasFileID || hasGuid || hasFilePath;
            const auto type = json.find("type");
            const bool hasNumericObjectReferenceType = type != json.end() && type->is_number_integer();
            const bool hasNonIntegerFileID = hasFileID && !json["fileID"].is_number_integer();
            const bool hasIncompleteObjectReferenceShape =
                hasObjectReferenceKey &&
                !(hasFileID &&
                    json["fileID"].is_number_integer() &&
                    hasGuid &&
                    type != json.end() &&
                    type->is_number_integer());
            const bool hasHalfShapedAssetReference =
                !hasFileID &&
                hasGuid &&
                hasNumericObjectReferenceType;
            const bool hasPathOnlyObjectReferenceHint =
                !hasFileID &&
                hasFilePath &&
                (hasGuid || hasNumericObjectReferenceType);

            return hasLegacyReference ||
                hasNonIntegerFileID ||
                hasIncompleteObjectReferenceShape ||
                hasHalfShapedAssetReference ||
                hasPathOnlyObjectReferenceHint;
        }

        static std::optional<PatchOperation> ReadPatchOperation(const nlohmann::json& json)
        {
            if (!json.is_object())
                return std::nullopt;

            const auto op = json.value("op", std::string {});
            if (op == "replaceProperty")
            {
                auto target = NLS::Guid::TryParse(json.value("target", std::string {}));
                const auto valueJson = json.find("value");
                if (!target.has_value() || valueJson == json.end())
                    return std::nullopt;

                auto value = ReadValue(*valueJson);
                if (!value.has_value())
                    return std::nullopt;

                return PatchOperation::ReplaceProperty(
                    ObjectId(*target),
                    json.value("property", std::string {}),
                    std::move(*value));
            }

            if (op == "insertOwned" || op == "removeOwned" || op == "moveOwned")
            {
                auto owner = NLS::Guid::TryParse(json.value("owner", std::string {}));
                auto object = NLS::Guid::TryParse(json.value("object", std::string {}));
                if (!owner.has_value() || !object.has_value())
                    return std::nullopt;

                const auto property = json.value("property", std::string {});
                if (op == "insertOwned")
                {
                    return PatchOperation::InsertOwned(
                        ObjectId(*owner),
                        property,
                        ObjectId(*object),
                        json.value("index", static_cast<size_t>(0)));
                }

                if (op == "removeOwned")
                    return PatchOperation::RemoveOwned(ObjectId(*owner), property, ObjectId(*object));

                return PatchOperation::MoveOwned(
                    ObjectId(*owner),
                    property,
                    ObjectId(*object),
                    json.value("index", static_cast<size_t>(0)));
            }

            if (op == "removeObject")
            {
                auto target = NLS::Guid::TryParse(json.value("target", std::string {}));
                if (!target.has_value())
                    return std::nullopt;

                PatchOperation operation;
                operation.type = PatchOperationType::RemoveObject;
                operation.target = ObjectId(*target);
                return operation;
            }

            return std::nullopt;
        }
    };
}
