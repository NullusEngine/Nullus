#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "Serialize/ObjectId.h"
#include "Serialize/PPtr.h"
#include "Serialize/SerializationDiagnostic.h"

namespace NLS::Engine::Serialize
{
    inline int64_t MakeLocalIdentifierInFile(const NLS::Guid& guid)
    {
        if (!guid.IsValid())
            return 0;

        uint64_t hash = 1469598103934665603ull;
        for (const auto byte : guid.GetBytes())
        {
            hash ^= static_cast<uint64_t>(byte);
            hash *= 1099511628211ull;
        }

        hash &= 0x7fffffffffffffffull;
        if (hash == 0)
            hash = 1;
        return static_cast<int64_t>(hash);
    }

    inline int64_t MakeLocalIdentifierInFile(const ObjectId& object)
    {
        return MakeLocalIdentifierInFile(object.GetGuid());
    }

    inline int64_t MakeLocalIdentifierInFile(const NLS::Guid& guid, std::string_view localIdentifierKey)
    {
        if (!guid.IsValid())
            return 0;

        uint64_t hash = 1469598103934665603ull;
        for (const auto byte : guid.GetBytes())
        {
            hash ^= static_cast<uint64_t>(byte);
            hash *= 1099511628211ull;
        }

        for (const auto character : localIdentifierKey)
        {
            hash ^= static_cast<uint64_t>(static_cast<unsigned char>(character));
            hash *= 1099511628211ull;
        }

        hash &= 0x7fffffffffffffffull;
        if (hash == 0)
            hash = 1;
        return static_cast<int64_t>(hash);
    }

    class PropertyValue
    {
    public:
        using ArrayValue = std::vector<PropertyValue>;
        using ObjectValue = std::vector<std::pair<std::string, PropertyValue>>;

        enum class Kind
        {
            Null,
            Bool,
            Integer,
            Number,
            String,
            Guid,
            OwnedReference,
            ObjectReference,
            Array,
            Object
        };

        PropertyValue() = default;

        static PropertyValue Null()
        {
            return {};
        }

        static PropertyValue Bool(bool value)
        {
            PropertyValue result;
            result.m_kind = Kind::Bool;
            result.m_value = value;
            return result;
        }

        static PropertyValue Integer(int64_t value)
        {
            PropertyValue result;
            result.m_kind = Kind::Integer;
            result.m_value = value;
            return result;
        }

        static PropertyValue Number(double value)
        {
            PropertyValue result;
            result.m_kind = Kind::Number;
            result.m_value = value;
            return result;
        }

        static PropertyValue String(std::string value)
        {
            PropertyValue result;
            result.m_kind = Kind::String;
            result.m_value = std::move(value);
            return result;
        }

        static PropertyValue Guid(NLS::Guid value)
        {
            PropertyValue result;
            result.m_kind = Kind::Guid;
            result.m_value = value;
            return result;
        }

        static PropertyValue OwnedReference(ObjectId value)
        {
            PropertyValue result;
            result.m_kind = Kind::OwnedReference;
            result.m_value = value;
            return result;
        }

        static PropertyValue ObjectReference(ObjectIdentifier value)
        {
            PropertyValue result;
            result.m_kind = Kind::ObjectReference;
            result.m_value = std::move(value);
            return result;
        }

        static PropertyValue Array(ArrayValue value)
        {
            PropertyValue result;
            result.m_kind = Kind::Array;
            result.m_value = std::move(value);
            return result;
        }

        static PropertyValue Object(ObjectValue value)
        {
            PropertyValue result;
            result.m_kind = Kind::Object;
            result.m_value = std::move(value);
            return result;
        }

        Kind GetKind() const
        {
            return m_kind;
        }

        const ObjectId& GetObjectId() const
        {
            return std::get<ObjectId>(m_value);
        }

        const ObjectIdentifier& GetObjectReference() const
        {
            return std::get<ObjectIdentifier>(m_value);
        }

        ObjectIdentifier& GetMutableObjectReference()
        {
            return std::get<ObjectIdentifier>(m_value);
        }

        bool GetBool() const
        {
            return std::get<bool>(m_value);
        }

        int64_t GetInteger() const
        {
            return std::get<int64_t>(m_value);
        }

        double GetNumber() const
        {
            return std::get<double>(m_value);
        }

        const std::string& GetString() const
        {
            return std::get<std::string>(m_value);
        }

        const NLS::Guid& GetGuid() const
        {
            return std::get<NLS::Guid>(m_value);
        }

        const ArrayValue& GetArray() const
        {
            return std::get<ArrayValue>(m_value);
        }

        const ObjectValue& GetObject() const
        {
            return std::get<ObjectValue>(m_value);
        }

    private:
        Kind m_kind = Kind::Null;
        std::variant<
            std::monostate,
            bool,
            int64_t,
            double,
            std::string,
            NLS::Guid,
            ObjectId,
            ObjectIdentifier,
            ArrayValue,
            ObjectValue> m_value;
    };

    struct PropertyRecord
    {
        std::string name;
        PropertyValue value;
    };

    enum class ObjectRecordState
    {
        Alive,
        Removed
    };

    struct ObjectRecord
    {
        ObjectRecord() = default;

        ObjectRecord(
            ObjectId objectId,
            std::string objectTypeName,
            std::string objectDebugName,
            std::string objectDebugPath,
            ObjectRecordState objectState,
            std::vector<PropertyRecord> objectProperties,
            int64_t objectLocalIdentifierInFile = 0)
            : id(std::move(objectId)),
              typeName(std::move(objectTypeName)),
              debugName(std::move(objectDebugName)),
              debugPath(std::move(objectDebugPath)),
              state(objectState),
              properties(std::move(objectProperties)),
              localIdentifierInFile(objectLocalIdentifierInFile)
        {
        }

        ObjectId id;
        std::string typeName;
        std::string debugName;
        std::string debugPath;
        ObjectRecordState state = ObjectRecordState::Alive;
        std::vector<PropertyRecord> properties;
        int64_t localIdentifierInFile = 0;
    };

    enum class PatchOperationType
    {
        ReplaceProperty,
        InsertOwned,
        RemoveOwned,
        MoveOwned,
        AddPrefabInstance,
        RemoveObject
    };

    struct PatchOperation
    {
        PatchOperationType type = PatchOperationType::ReplaceProperty;
        ObjectId target;
        std::string property;
        PropertyValue value;
        ObjectId object;
        size_t index = 0;
        bool hasIndex = false;

        static PatchOperation ReplaceProperty(ObjectId target, std::string property, PropertyValue value)
        {
            PatchOperation operation;
            operation.type = PatchOperationType::ReplaceProperty;
            operation.target = target;
            operation.property = std::move(property);
            operation.value = std::move(value);
            return operation;
        }

        static PatchOperation InsertOwned(ObjectId owner, std::string property, ObjectId object, size_t index)
        {
            PatchOperation operation;
            operation.type = PatchOperationType::InsertOwned;
            operation.target = owner;
            operation.property = std::move(property);
            operation.object = object;
            operation.index = index;
            operation.hasIndex = true;
            return operation;
        }

        static PatchOperation RemoveOwned(ObjectId owner, std::string property, ObjectId object)
        {
            PatchOperation operation;
            operation.type = PatchOperationType::RemoveOwned;
            operation.target = owner;
            operation.property = std::move(property);
            operation.object = object;
            return operation;
        }

        static PatchOperation MoveOwned(ObjectId owner, std::string property, ObjectId object, size_t index)
        {
            PatchOperation operation;
            operation.type = PatchOperationType::MoveOwned;
            operation.target = owner;
            operation.property = std::move(property);
            operation.object = object;
            operation.index = index;
            operation.hasIndex = true;
            return operation;
        }
    };

    class ObjectGraphDocument
    {
    public:
        std::string format = "Nullus.ObjectGraph.Scene";
        int version = 1;
        NLS::Guid documentId;
        ObjectId root;
        std::optional<ObjectIdentifier> basePrefab;
        std::vector<ObjectRecord> objects;
        std::vector<PatchOperation> overrides;

        static ObjectIdentifier MakeLocalObjectReference(const ObjectRecord& object)
        {
            return ObjectIdentifier::LocalObject(object.localIdentifierInFile);
        }

        int64_t GetLocalIdentifierInFileForObject(const ObjectId& id) const
        {
            for (const auto& object : objects)
            {
                if (object.id == id)
                    return object.localIdentifierInFile;
            }
            return 0;
        }

        std::optional<ObjectIdentifier> TryMakeObjectReference(const ObjectId& id) const
        {
            const auto localIdentifierInFile = GetLocalIdentifierInFileForObject(id);
            if (localIdentifierInFile == 0)
                return std::nullopt;
            return ObjectIdentifier::LocalObject(localIdentifierInFile);
        }

        std::optional<ObjectId> ResolveObjectReference(const ObjectIdentifier& reference) const
        {
            if (reference.localIdentifierInFile == 0 || reference.guid.IsValid())
                return std::nullopt;

            for (const auto& object : objects)
            {
                if (object.state == ObjectRecordState::Removed)
                    continue;
                if (object.localIdentifierInFile == reference.localIdentifierInFile)
                    return object.id;
            }
            return std::nullopt;
        }

        SerializationDiagnosticList Validate() const
        {
            SerializationDiagnosticList diagnostics;
            std::unordered_map<ObjectId, size_t> indexById;
            std::unordered_map<int64_t, ObjectId> objectIdByFileID;

            for (size_t index = 0; index < objects.size(); ++index)
            {
                const auto& object = objects[index];
                if (object.state == ObjectRecordState::Removed)
                    continue;

                if (!object.id.IsValid())
                {
                    AddError(diagnostics, SerializationDiagnosticCode::InvalidGuid, "Object record has an invalid id.");
                    continue;
                }

                if (!indexById.emplace(object.id, index).second)
                {
                    AddError(diagnostics, SerializationDiagnosticCode::DuplicateObjectId, "Object graph contains duplicate object ids.");
                }

                if (object.localIdentifierInFile == 0)
                {
                    AddError(diagnostics, SerializationDiagnosticCode::InvalidPropertyType, "Object record has a missing fileID.");
                }
                else if (!objectIdByFileID.emplace(object.localIdentifierInFile, object.id).second)
                {
                    AddError(diagnostics, SerializationDiagnosticCode::DuplicateObjectId, "Object graph contains duplicate fileID values.");
                }
            }

            if (!root.IsValid() || indexById.find(root) == indexById.end())
            {
                AddError(diagnostics, SerializationDiagnosticCode::MissingObject, "Object graph root is missing.");
            }

            if (basePrefab.has_value())
                ValidateAssetReference(*basePrefab, diagnostics);

            for (const auto& object : objects)
            {
                if (object.state == ObjectRecordState::Removed)
                    continue;

                for (const auto& property : object.properties)
                    ValidateReferences(property.value, indexById, objectIdByFileID, diagnostics);
            }

            if (root.IsValid() && indexById.find(root) != indexById.end())
                ValidateOwnership(indexById, diagnostics);

            return diagnostics;
        }

    private:
        static void AddError(
            SerializationDiagnosticList& diagnostics,
            SerializationDiagnosticCode code,
            std::string message)
        {
            diagnostics.Add({code, SerializationDiagnosticSeverity::Error, std::move(message)});
        }

        static void ValidateAssetReference(
            const ObjectIdentifier& reference,
            SerializationDiagnosticList& diagnostics)
        {
            if (!IsKnownFileType(reference.fileType))
            {
                AddError(diagnostics, SerializationDiagnosticCode::MissingAsset, "Object graph contains an invalid object reference file type.");
                return;
            }

            if (!reference.guid.IsValid() ||
                reference.localIdentifierInFile == 0 ||
                reference.fileType == FileType::NonAssetType)
            {
                AddError(diagnostics, SerializationDiagnosticCode::MissingAsset, "Object graph contains an invalid asset reference.");
            }
        }

        static void ValidateReferences(
            const PropertyValue& value,
            const std::unordered_map<ObjectId, size_t>& indexById,
            const std::unordered_map<int64_t, ObjectId>& objectIdByFileID,
            SerializationDiagnosticList& diagnostics)
        {
            switch (value.GetKind())
            {
            case PropertyValue::Kind::OwnedReference:
                if (!value.GetObjectId().IsValid() || indexById.find(value.GetObjectId()) == indexById.end())
                {
                    AddError(diagnostics, SerializationDiagnosticCode::DanglingReference, "Object graph contains a missing object reference.");
                }
                break;
            case PropertyValue::Kind::ObjectReference:
            {
                const auto& reference = value.GetObjectReference();
                if (!IsKnownFileType(reference.fileType))
                {
                    AddError(diagnostics, SerializationDiagnosticCode::MissingAsset, "Object graph contains an invalid object reference file type.");
                    break;
                }
                if (!reference.IsValid())
                {
                    if (reference.guid.IsValid() ||
                        reference.fileType != FileType::NonAssetType ||
                        !reference.filePath.empty())
                    {
                        AddError(diagnostics, SerializationDiagnosticCode::MissingAsset, "Object graph contains an invalid asset reference.");
                    }
                    break;
                }
                if (reference.guid.IsValid())
                {
                    if (reference.localIdentifierInFile == 0 ||
                        reference.fileType == FileType::NonAssetType)
                        AddError(diagnostics, SerializationDiagnosticCode::MissingAsset, "Object graph contains an invalid asset reference.");
                    break;
                }
                if (reference.fileType != FileType::NonAssetType ||
                    !reference.filePath.empty())
                {
                    AddError(diagnostics, SerializationDiagnosticCode::MissingAsset, "Object graph contains an invalid asset reference.");
                    break;
                }
                if (reference.localIdentifierInFile == 0 || objectIdByFileID.find(reference.localIdentifierInFile) == objectIdByFileID.end())
                    AddError(diagnostics, SerializationDiagnosticCode::DanglingReference, "Object graph contains a missing object reference.");
                break;
            }
            case PropertyValue::Kind::Array:
                for (const auto& item : value.GetArray())
                    ValidateReferences(item, indexById, objectIdByFileID, diagnostics);
                break;
            case PropertyValue::Kind::Object:
                for (const auto& property : value.GetObject())
                    ValidateReferences(property.second, indexById, objectIdByFileID, diagnostics);
                break;
            default:
                break;
            }
        }

        void ValidateOwnership(
            const std::unordered_map<ObjectId, size_t>& indexById,
            SerializationDiagnosticList& diagnostics) const
        {
            std::unordered_map<ObjectId, std::vector<ObjectId>> ownedChildren;
            for (const auto& object : objects)
            {
                if (object.state == ObjectRecordState::Removed)
                    continue;

                for (const auto& property : object.properties)
                    CollectOwnedReferences(object.id, property.value, ownedChildren);
            }

            std::unordered_set<ObjectId> reachable;
            std::unordered_set<ObjectId> visiting;
            TraverseOwnership(root, ownedChildren, reachable, visiting, diagnostics);

            for (const auto& object : objects)
            {
                if (object.state == ObjectRecordState::Removed)
                    continue;

                if (!object.id.IsValid())
                    continue;

                if (reachable.find(object.id) == reachable.end())
                    AddError(diagnostics, SerializationDiagnosticCode::OrphanedOwnedObject, "Object graph contains an owned object unreachable from root.");
            }
        }

        static void CollectOwnedReferences(
            const ObjectId& owner,
            const PropertyValue& value,
            std::unordered_map<ObjectId, std::vector<ObjectId>>& ownedChildren)
        {
            switch (value.GetKind())
            {
            case PropertyValue::Kind::OwnedReference:
                ownedChildren[owner].push_back(value.GetObjectId());
                break;
            case PropertyValue::Kind::Array:
                for (const auto& item : value.GetArray())
                    CollectOwnedReferences(owner, item, ownedChildren);
                break;
            case PropertyValue::Kind::Object:
                for (const auto& property : value.GetObject())
                    CollectOwnedReferences(owner, property.second, ownedChildren);
                break;
            default:
                break;
            }
        }

        static void TraverseOwnership(
            const ObjectId& id,
            const std::unordered_map<ObjectId, std::vector<ObjectId>>& ownedChildren,
            std::unordered_set<ObjectId>& reachable,
            std::unordered_set<ObjectId>& visiting,
            SerializationDiagnosticList& diagnostics)
        {
            if (visiting.find(id) != visiting.end())
            {
                AddError(diagnostics, SerializationDiagnosticCode::OwnershipCycle, "Object graph contains an ownership cycle.");
                return;
            }

            if (!reachable.insert(id).second)
                return;

            visiting.insert(id);
            const auto foundChildren = ownedChildren.find(id);
            if (foundChildren != ownedChildren.end())
            {
                for (const auto& childId : foundChildren->second)
                    TraverseOwnership(childId, ownedChildren, reachable, visiting, diagnostics);
            }
            visiting.erase(id);
        }
    };
}
