#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Serialize/ObjectGraphDocument.h"

namespace NLS::Engine::Serialize
{
    class ObjectGraphBinaryCache
    {
    public:
        static std::vector<uint8_t> Write(const ObjectGraphDocument& document)
        {
            Writer writer;
            writer.WriteBytes(kMagic, sizeof(kMagic));
            writer.WriteU32(kVersion);
            writer.WriteString(document.format);
            writer.WriteI32(document.version);
            writer.WriteGuid(document.documentId);
            writer.WriteObjectId(document.root);
            writer.WriteBool(document.basePrefab.has_value());
            if (document.basePrefab.has_value())
                writer.WriteObjectIdentifier(*document.basePrefab);

            writer.WriteVector(document.objects, [](Writer& output, const ObjectRecord& object)
            {
                output.WriteObjectRecord(object);
            });
            writer.WriteVector(document.overrides, [](Writer& output, const PatchOperation& operation)
            {
                output.WritePatchOperation(operation);
            });
            writer.WriteVector(document.prefabInstances, [](Writer& output, const PrefabInstanceRecord& instance)
            {
                output.WritePrefabInstance(instance);
            });
            return std::move(writer.bytes);
        }

        static std::optional<ObjectGraphDocument> Read(const uint8_t* data, const size_t size)
        {
            if (data == nullptr && size != 0u)
                return std::nullopt;

            Reader reader(data, size);
            if (!reader.ReadMagic())
                return std::nullopt;

            uint32_t version = 0u;
            if (!reader.ReadU32(version) || version < kMinVersion || version > kVersion)
                return std::nullopt;

            ObjectGraphDocument document;
            if (!reader.ReadString(document.format) ||
                !reader.ReadI32(document.version) ||
                !reader.ReadGuid(document.documentId) ||
                !reader.ReadObjectId(document.root))
            {
                return std::nullopt;
            }

            bool hasBasePrefab = false;
            if (!reader.ReadBool(hasBasePrefab))
                return std::nullopt;
            if (hasBasePrefab)
            {
                ObjectIdentifier basePrefab;
                if (!reader.ReadObjectIdentifier(basePrefab) ||
                    !basePrefab.guid.IsValid())
                {
                    return std::nullopt;
                }
                document.basePrefab = std::move(basePrefab);
            }

            if (!reader.ReadVector(document.objects, [](Reader& input, ObjectRecord& object)
                {
                    return input.ReadObjectRecord(object);
                }) ||
                !reader.ReadVector(document.overrides, [](Reader& input, PatchOperation& operation)
                {
                    return input.ReadPatchOperation(operation);
                }) ||
                !reader.ReadVector(document.prefabInstances, [](Reader& input, PrefabInstanceRecord& instance)
                {
                    return input.ReadPrefabInstance(instance);
                }))
            {
                return std::nullopt;
            }

            if (!reader.IsComplete())
                return std::nullopt;

            return document;
        }

        static std::optional<ObjectGraphDocument> Read(const std::vector<uint8_t>& bytes)
        {
            return Read(bytes.data(), bytes.size());
        }

    private:
        static constexpr uint8_t kMagic[] = {'N', 'O', 'G', 'C'};
        static constexpr uint32_t kMinVersion = 1u;
        static constexpr uint32_t kVersion = 1u;

        class Writer
        {
        public:
            std::vector<uint8_t> bytes;

            void WriteBytes(const void* data, const size_t size)
            {
                const auto* begin = static_cast<const uint8_t*>(data);
                bytes.insert(bytes.end(), begin, begin + size);
            }

            void WriteBool(const bool value)
            {
                bytes.push_back(value ? 1u : 0u);
            }

            void WriteU8(const uint8_t value)
            {
                bytes.push_back(value);
            }

            void WriteU32(const uint32_t value)
            {
                for (uint32_t shift = 0u; shift < 32u; shift += 8u)
                    bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
            }

            void WriteU64(const uint64_t value)
            {
                for (uint32_t shift = 0u; shift < 64u; shift += 8u)
                    bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
            }

            void WriteI32(const int32_t value)
            {
                WriteU32(static_cast<uint32_t>(value));
            }

            void WriteI64(const int64_t value)
            {
                WriteU64(static_cast<uint64_t>(value));
            }

            void WriteDouble(const double value)
            {
                uint64_t bits = 0u;
                static_assert(sizeof(bits) == sizeof(value));
                std::memcpy(&bits, &value, sizeof(bits));
                WriteU64(bits);
            }

            void WriteString(const std::string& value)
            {
                WriteU32(static_cast<uint32_t>(value.size()));
                WriteBytes(value.data(), value.size());
            }

            void WriteGuid(const NLS::Guid& guid)
            {
                WriteBytes(guid.GetBytes().data(), guid.GetBytes().size());
            }

            void WriteObjectId(const ObjectId& object)
            {
                WriteGuid(object.GetGuid());
            }

            void WriteObjectIdentifier(const ObjectIdentifier& identifier)
            {
                WriteGuid(identifier.guid);
                WriteI64(identifier.localIdentifierInFile);
                WriteI32(static_cast<int32_t>(identifier.fileType));
                WriteString(identifier.filePath);
            }

            template <typename T, typename Callback>
            void WriteVector(const std::vector<T>& values, Callback callback)
            {
                WriteU32(static_cast<uint32_t>(values.size()));
                for (const auto& value : values)
                    callback(*this, value);
            }

            void WritePropertyValue(const PropertyValue& value)
            {
                WriteU8(static_cast<uint8_t>(value.GetKind()));
                switch (value.GetKind())
                {
                case PropertyValue::Kind::Null:
                    break;
                case PropertyValue::Kind::Bool:
                    WriteBool(value.GetBool());
                    break;
                case PropertyValue::Kind::Integer:
                    WriteI64(value.GetInteger());
                    break;
                case PropertyValue::Kind::Number:
                    WriteDouble(value.GetNumber());
                    break;
                case PropertyValue::Kind::String:
                    WriteString(value.GetString());
                    break;
                case PropertyValue::Kind::Guid:
                    WriteGuid(value.GetGuid());
                    break;
                case PropertyValue::Kind::OwnedReference:
                    WriteObjectId(value.GetObjectId());
                    break;
                case PropertyValue::Kind::ObjectReference:
                    WriteObjectIdentifier(value.GetObjectReference());
                    break;
                case PropertyValue::Kind::Array:
                    WriteVector(value.GetArray(), [](Writer& output, const PropertyValue& item)
                    {
                        output.WritePropertyValue(item);
                    });
                    break;
                case PropertyValue::Kind::Object:
                    WriteU32(static_cast<uint32_t>(value.GetObject().size()));
                    for (const auto& [name, item] : value.GetObject())
                    {
                        WriteString(name);
                        WritePropertyValue(item);
                    }
                    break;
                }
            }

            void WritePropertyRecord(const PropertyRecord& property)
            {
                WriteString(property.name);
                WritePropertyValue(property.value);
            }

            void WriteObjectRecord(const ObjectRecord& object)
            {
                WriteObjectId(object.id);
                WriteString(object.typeName);
                WriteString(object.debugName);
                WriteString(object.debugPath);
                WriteU8(static_cast<uint8_t>(object.state));
                WriteI64(object.localIdentifierInFile);
                WriteVector(object.properties, [](Writer& output, const PropertyRecord& property)
                {
                    output.WritePropertyRecord(property);
                });
            }

            void WritePatchOperation(const PatchOperation& operation)
            {
                WriteU8(static_cast<uint8_t>(operation.type));
                WriteObjectId(operation.target);
                WriteString(operation.property);
                WritePropertyValue(operation.value);
                WriteObjectId(operation.object);
                WriteU64(static_cast<uint64_t>(operation.index));
                WriteBool(operation.hasIndex);
            }

            void WritePrefabInstance(const PrefabInstanceRecord& instance)
            {
                WriteObjectId(instance.instanceRoot);
                WriteObjectIdentifier(instance.sourcePrefab);
                WriteBool(instance.generatedReadOnly);
                WriteVector(instance.modifications, [](Writer& output, const PatchOperation& operation)
                {
                    output.WritePatchOperation(operation);
                });
                WriteVector(instance.addedObjects, [](Writer& output, const ObjectRecord& object)
                {
                    output.WriteObjectRecord(object);
                });
                WriteVector(instance.correspondence, [](Writer& output, const PrefabInstanceObjectCorrespondence& mapping)
                {
                    output.WriteObjectId(mapping.sourceObject);
                    output.WriteObjectId(mapping.instanceObject);
                });
            }
        };

        class Reader
        {
        public:
            Reader(const uint8_t* input, const size_t inputSize)
                : data(input)
                , size(inputSize)
            {
            }

            bool ReadMagic()
            {
                if (Remaining() < sizeof(kMagic))
                    return false;
                if (std::memcmp(data + position, kMagic, sizeof(kMagic)) != 0)
                    return false;
                position += sizeof(kMagic);
                return true;
            }

            bool IsComplete() const
            {
                return position == size;
            }

            bool ReadBool(bool& value)
            {
                uint8_t byte = 0u;
                if (!ReadU8(byte) || byte > 1u)
                    return false;
                value = byte != 0u;
                return true;
            }

            bool ReadU8(uint8_t& value)
            {
                if (Remaining() < 1u)
                    return false;
                value = data[position++];
                return true;
            }

            bool ReadU32(uint32_t& value)
            {
                if (Remaining() < 4u)
                    return false;
                value =
                    static_cast<uint32_t>(data[position]) |
                    (static_cast<uint32_t>(data[position + 1u]) << 8u) |
                    (static_cast<uint32_t>(data[position + 2u]) << 16u) |
                    (static_cast<uint32_t>(data[position + 3u]) << 24u);
                position += 4u;
                return true;
            }

            bool ReadU64(uint64_t& value)
            {
                if (Remaining() < 8u)
                    return false;
                value =
                    static_cast<uint64_t>(data[position]) |
                    (static_cast<uint64_t>(data[position + 1u]) << 8u) |
                    (static_cast<uint64_t>(data[position + 2u]) << 16u) |
                    (static_cast<uint64_t>(data[position + 3u]) << 24u) |
                    (static_cast<uint64_t>(data[position + 4u]) << 32u) |
                    (static_cast<uint64_t>(data[position + 5u]) << 40u) |
                    (static_cast<uint64_t>(data[position + 6u]) << 48u) |
                    (static_cast<uint64_t>(data[position + 7u]) << 56u);
                position += 8u;
                return true;
            }

            bool ReadI32(int32_t& value)
            {
                uint32_t raw = 0u;
                if (!ReadU32(raw))
                    return false;
                value = static_cast<int32_t>(raw);
                return true;
            }

            bool ReadI64(int64_t& value)
            {
                uint64_t raw = 0u;
                if (!ReadU64(raw))
                    return false;
                value = static_cast<int64_t>(raw);
                return true;
            }

            bool ReadDouble(double& value)
            {
                uint64_t bits = 0u;
                if (!ReadU64(bits))
                    return false;
                static_assert(sizeof(bits) == sizeof(value));
                std::memcpy(&value, &bits, sizeof(value));
                return true;
            }

            bool ReadString(std::string& value)
            {
                uint32_t length = 0u;
                if (!ReadU32(length) || Remaining() < length)
                    return false;
                value.assign(reinterpret_cast<const char*>(data + position), length);
                position += length;
                return true;
            }

            bool ReadGuid(NLS::Guid& guid)
            {
                if (Remaining() < NLS::Guid::Bytes {}.size())
                    return false;
                NLS::Guid::Bytes bytes {};
                std::memcpy(bytes.data(), data + position, bytes.size());
                position += bytes.size();
                guid = NLS::Guid(bytes);
                return true;
            }

            bool ReadObjectId(ObjectId& object)
            {
                NLS::Guid guid;
                if (!ReadGuid(guid))
                    return false;
                object = ObjectId(guid);
                return true;
            }

            bool ReadObjectIdentifier(ObjectIdentifier& identifier)
            {
                int32_t fileType = 0;
                if (!ReadGuid(identifier.guid) ||
                    !ReadI64(identifier.localIdentifierInFile) ||
                    !ReadI32(fileType) ||
                    !ReadString(identifier.filePath))
                {
                    return false;
                }

                identifier.fileType = static_cast<FileType>(fileType);
                return IsKnownFileType(identifier.fileType) &&
                    IsValidSerializedObjectReference(identifier);
            }

            template <typename T, typename Callback>
            bool ReadVector(std::vector<T>& values, Callback callback)
            {
                uint32_t count = 0u;
                if (!ReadU32(count))
                    return false;
                if (count > kMaxUncheckedCollectionElementCount && count > Remaining())
                    return false;

                values.clear();
                values.reserve(count);
                for (uint32_t index = 0u; index < count; ++index)
                {
                    auto& value = values.emplace_back();
                    if (!callback(*this, value))
                    {
                        values.clear();
                        return false;
                    }
                }
                return true;
            }

            bool ReadPropertyValue(PropertyValue& value)
            {
                uint8_t kindValue = 0u;
                if (!ReadU8(kindValue) || kindValue > static_cast<uint8_t>(PropertyValue::Kind::Object))
                    return false;

                const auto kind = static_cast<PropertyValue::Kind>(kindValue);
                switch (kind)
                {
                case PropertyValue::Kind::Null:
                    value = PropertyValue::Null();
                    return true;
                case PropertyValue::Kind::Bool:
                {
                    bool boolValue = false;
                    if (!ReadBool(boolValue))
                        return false;
                    value = PropertyValue::Bool(boolValue);
                    return true;
                }
                case PropertyValue::Kind::Integer:
                {
                    int64_t integerValue = 0;
                    if (!ReadI64(integerValue))
                        return false;
                    value = PropertyValue::Integer(integerValue);
                    return true;
                }
                case PropertyValue::Kind::Number:
                {
                    double numberValue = 0.0;
                    if (!ReadDouble(numberValue))
                        return false;
                    value = PropertyValue::Number(numberValue);
                    return true;
                }
                case PropertyValue::Kind::String:
                {
                    std::string stringValue;
                    if (!ReadString(stringValue))
                        return false;
                    value = PropertyValue::String(std::move(stringValue));
                    return true;
                }
                case PropertyValue::Kind::Guid:
                {
                    NLS::Guid guidValue;
                    if (!ReadGuid(guidValue))
                        return false;
                    value = PropertyValue::Guid(guidValue);
                    return true;
                }
                case PropertyValue::Kind::OwnedReference:
                {
                    ObjectId objectId;
                    if (!ReadObjectId(objectId))
                        return false;
                    value = PropertyValue::OwnedReference(objectId);
                    return true;
                }
                case PropertyValue::Kind::ObjectReference:
                {
                    ObjectIdentifier reference;
                    if (!ReadObjectIdentifier(reference))
                        return false;
                    value = PropertyValue::ObjectReference(std::move(reference));
                    return true;
                }
                case PropertyValue::Kind::Array:
                {
                    PropertyValue::ArrayValue array;
                    if (!ReadVector(array, [](Reader& input, PropertyValue& item)
                        {
                            return input.ReadPropertyValue(item);
                        }))
                    {
                        return false;
                    }
                    value = PropertyValue::Array(std::move(array));
                    return true;
                }
                case PropertyValue::Kind::Object:
                {
                    uint32_t count = 0u;
                    if (!ReadU32(count))
                        return false;
                    if (count > kMaxUncheckedCollectionElementCount && count > Remaining())
                        return false;
                    PropertyValue::ObjectValue object;
                    object.reserve(count);
                    for (uint32_t index = 0u; index < count; ++index)
                    {
                        auto& [name, item] = object.emplace_back();
                        if (!ReadString(name) || !ReadPropertyValue(item))
                        {
                            object.clear();
                            return false;
                        }
                    }
                    value = PropertyValue::Object(std::move(object));
                    return true;
                }
                }
                return false;
            }

            bool ReadPropertyRecord(PropertyRecord& property)
            {
                return ReadString(property.name) &&
                    ReadPropertyValue(property.value);
            }

            bool ReadObjectRecord(ObjectRecord& object)
            {
                uint8_t state = 0u;
                if (!ReadObjectId(object.id) ||
                    !ReadString(object.typeName) ||
                    !ReadString(object.debugName) ||
                    !ReadString(object.debugPath) ||
                    !ReadU8(state) ||
                    state > static_cast<uint8_t>(ObjectRecordState::Removed) ||
                    !ReadI64(object.localIdentifierInFile))
                {
                    return false;
                }
                object.state = static_cast<ObjectRecordState>(state);
                return ReadVector(object.properties, [](Reader& input, PropertyRecord& property)
                {
                    return input.ReadPropertyRecord(property);
                });
            }

            bool ReadPatchOperation(PatchOperation& operation)
            {
                uint8_t type = 0u;
                uint64_t index = 0u;
                if (!ReadU8(type) ||
                    type > static_cast<uint8_t>(PatchOperationType::RemoveObject) ||
                    !ReadObjectId(operation.target) ||
                    !ReadString(operation.property) ||
                    !ReadPropertyValue(operation.value) ||
                    !ReadObjectId(operation.object) ||
                    !ReadU64(index) ||
                    index > std::numeric_limits<size_t>::max() ||
                    !ReadBool(operation.hasIndex))
                {
                    return false;
                }
                operation.type = static_cast<PatchOperationType>(type);
                operation.index = static_cast<size_t>(index);
                return true;
            }

            bool ReadPrefabInstance(PrefabInstanceRecord& instance)
            {
                return ReadObjectId(instance.instanceRoot) &&
                    ReadObjectIdentifier(instance.sourcePrefab) &&
                    ReadBool(instance.generatedReadOnly) &&
                    ReadVector(instance.modifications, [](Reader& input, PatchOperation& operation)
                    {
                        return input.ReadPatchOperation(operation);
                    }) &&
                    ReadVector(instance.addedObjects, [](Reader& input, ObjectRecord& object)
                    {
                        return input.ReadObjectRecord(object);
                    }) &&
                    ReadVector(instance.correspondence, [](Reader& input, PrefabInstanceObjectCorrespondence& mapping)
                    {
                        return input.ReadObjectId(mapping.sourceObject) &&
                            input.ReadObjectId(mapping.instanceObject);
                    });
            }

        private:
            size_t Remaining() const
            {
                return size - position;
            }

            static bool IsValidSerializedObjectReference(const ObjectIdentifier& reference)
            {
                if (reference.guid.IsValid())
                {
                    return reference.localIdentifierInFile != 0 &&
                        reference.fileType != FileType::NonAssetType;
                }

                if (reference.localIdentifierInFile == 0)
                {
                    return reference.fileType == FileType::NonAssetType &&
                        reference.filePath.empty();
                }

                return reference.fileType == FileType::NonAssetType &&
                    reference.filePath.empty();
            }

            const uint8_t* data = nullptr;
            size_t size = 0u;
            size_t position = 0u;
            static constexpr uint32_t kMaxUncheckedCollectionElementCount = 4096u;
        };
    };
}
