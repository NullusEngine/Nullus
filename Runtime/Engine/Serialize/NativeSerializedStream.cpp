#include "Serialize/NativeSerializedStream.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <exception>
#include <limits>
#include <sstream>

#include <Math/Color.h>
#include <Math/Bounds.h>
#include <Math/BoundsInt.h>
#include <Math/Quaternion.h>
#include <Math/Rect.h>
#include <Math/RectInt.h>
#include <Math/Vector2.h>
#include <Math/Vector2Int.h>
#include <Math/Vector3.h>
#include <Math/Vector3Int.h>
#include <Math/Vector4.h>
#include <Reflection/Argument.h>
#include <Reflection/ArrayWrapper.h>
#include <Reflection/Constructor.h>
#include <Reflection/Enum.h>
#include <Reflection/Field.h>
#include <Reflection/VariantPolicy.h>

#include "Components/Component.h"
#include "GameObject.h"
#include "Object/Object.h"
#include "ObjectGraphPPtr.h"
#include "SerializedSpecialTypes.h"

namespace NLS::Engine::Serialize
{
namespace
{
constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

uint64_t HashBytes(uint64_t hash, const void* data, const size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t index = 0u; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= kFnvPrime;
    }
    return hash;
}

uint64_t HashString(uint64_t hash, const std::string_view value)
{
    return HashBytes(hash, value.data(), value.size());
}

uint64_t StableFieldId(const meta::Type& owner, const std::string_view field)
{
    uint64_t hash = HashString(kFnvOffset, owner.GetName());
    hash = HashString(hash, "|");
    hash = HashString(hash, field);
    return hash == 0u ? 1u : hash;
}

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool IsStringType(const meta::Type& type)
{
    const auto name = Lower(type.GetName());
    return name == "string" || name == "std::string" || name == "basic_string<char>" ||
        name.find("basic_string<char") != std::string::npos;
}

bool IsObjectReferenceType(const meta::Type& type)
{
    const auto name = type.GetName();
    return type.IsPointer() || name.rfind("NLS::Engine::Serialize::PPtr<", 0u) == 0u;
}

SerializedNumericKind NumericKindFor(const meta::Type& type)
{
    const auto name = Lower(type.GetName());
    if (name == "int8_t" || name == "signed char" || name == "sbyte") return SerializedNumericKind::Int8;
    if (name == "uint8_t" || name == "unsigned char" || name == "byte") return SerializedNumericKind::UInt8;
    if (name == "int16_t" || name == "short") return SerializedNumericKind::Int16;
    if (name == "uint16_t" || name == "unsigned short") return SerializedNumericKind::UInt16;
    if (name == "int32_t" || name == "int") return SerializedNumericKind::Int32;
    if (name == "uint32_t" || name == "unsigned int") return SerializedNumericKind::UInt32;
    if (name == "int64_t" || name == "long" || name == "long long") return SerializedNumericKind::Int64;
    if (name == "uint64_t" || name == "unsigned long" || name == "unsigned long long") return SerializedNumericKind::UInt64;
    if (name == "char" || name == "wchar_t" || name == "char16_t" || name == "char32_t") return SerializedNumericKind::Character;
    if (name == "float") return SerializedNumericKind::Float32;
    if (name == "double") return SerializedNumericKind::Float64;
    if (type.IsEnum())
    {
        const auto underlying = type.GetEnum().GetUnderlyingType();
        return underlying.IsValid() && underlying != type
            ? NumericKindFor(underlying)
            : SerializedNumericKind::Int32;
    }
    return SerializedNumericKind::Unknown;
}

size_t NumericByteSize(const SerializedNumericKind kind)
{
    switch (kind)
    {
    case SerializedNumericKind::Int8:
    case SerializedNumericKind::UInt8:
        return 1u;
    case SerializedNumericKind::Int16:
    case SerializedNumericKind::UInt16:
        return 2u;
    case SerializedNumericKind::Int32:
    case SerializedNumericKind::UInt32:
    case SerializedNumericKind::Float32:
        return 4u;
    case SerializedNumericKind::Int64:
    case SerializedNumericKind::UInt64:
    case SerializedNumericKind::Float64:
        return 8u;
    case SerializedNumericKind::Character:
        return 4u;
    default:
        return 0u;
    }
}

SerializedNodeKind NodeKindFor(const meta::Type& type)
{
    if (type.IsArray()) return SerializedNodeKind::Generic;
    if (IsStringType(type)) return SerializedNodeKind::String;
    if (IsObjectReferenceType(type)) return SerializedNodeKind::ObjectReference;
    if (type.IsEnum()) return SerializedNodeKind::Enum;

    const auto name = Lower(type.GetName());
    if (name.find("animationcurve") != std::string::npos)
        return SerializedNodeKind::AnimationCurve;
    if (name.find("gradient") != std::string::npos)
        return SerializedNodeKind::Gradient;
    if (name.find("exposedreference") != std::string::npos)
        return SerializedNodeKind::ExposedReference;
    if (name.find("vector2int") != std::string::npos || name.find("int2") != std::string::npos)
        return SerializedNodeKind::Vector2Int;
    if (name.find("vector3int") != std::string::npos || name.find("int3") != std::string::npos)
        return SerializedNodeKind::Vector3Int;
    if (name.find("vector2") != std::string::npos) return SerializedNodeKind::Vector2;
    if (name.find("vector3") != std::string::npos) return SerializedNodeKind::Vector3;
    if (name.find("vector4") != std::string::npos) return SerializedNodeKind::Vector4;
    if (name.find("quaternion") != std::string::npos) return SerializedNodeKind::Quaternion;
    if (name.find("color") != std::string::npos) return SerializedNodeKind::Color;
    if (name.find("rectint") != std::string::npos) return SerializedNodeKind::RectInt;
    if (name.find("rect") != std::string::npos) return SerializedNodeKind::Rect;
    if (name.find("boundsint") != std::string::npos) return SerializedNodeKind::BoundsInt;
    if (name.find("bounds") != std::string::npos) return SerializedNodeKind::Bounds;
    if (type == NLS_TYPEOF(bool)) return SerializedNodeKind::Boolean;
    if (type.IsFloatingPoint()) return SerializedNodeKind::Float;
    if (type.IsPrimitive()) return SerializedNodeKind::Integer;
    return SerializedNodeKind::Generic;
}

void SetNodeLayout(SerializedTypeTreeNode& node, const meta::Type& type)
{
    node.kind = NodeKindFor(type);
    node.numericKind = NumericKindFor(type);
    node.typeName = type.GetName();
    node.typeId = type.GetID();
    node.fixedByteSize = -1;

    switch (node.kind)
    {
    case SerializedNodeKind::Boolean:
        node.numericKind = SerializedNumericKind::UInt8;
        node.fixedByteSize = 1;
        break;
    case SerializedNodeKind::Integer:
    case SerializedNodeKind::Enum:
        node.fixedByteSize = static_cast<int32_t>(NumericByteSize(node.numericKind));
        break;
    case SerializedNodeKind::Float:
        node.fixedByteSize = static_cast<int32_t>(NumericByteSize(node.numericKind));
        break;
    case SerializedNodeKind::ObjectReference:
        node.fixedByteSize = 4;
        node.flags = node.flags | SerializedNodeFlags::HasObjectReference;
        break;
    case SerializedNodeKind::Vector2:
        node.fixedByteSize = 8;
        break;
    case SerializedNodeKind::Vector3:
        node.fixedByteSize = 12;
        break;
    case SerializedNodeKind::Vector4:
    case SerializedNodeKind::Quaternion:
    case SerializedNodeKind::Color:
        node.fixedByteSize = 16;
        break;
    case SerializedNodeKind::Rect:
        node.fixedByteSize = 16;
        break;
    case SerializedNodeKind::Vector2Int:
        node.numericKind = SerializedNumericKind::Int32;
        node.fixedByteSize = 8;
        break;
    case SerializedNodeKind::Vector3Int:
        node.numericKind = SerializedNumericKind::Int32;
        node.fixedByteSize = 12;
        break;
    case SerializedNodeKind::RectInt:
    case SerializedNodeKind::BoundsInt:
        node.numericKind = SerializedNumericKind::Int32;
        node.fixedByteSize = node.kind == SerializedNodeKind::RectInt ? 16 : 24;
        break;
    case SerializedNodeKind::Bounds:
        node.fixedByteSize = 24;
        break;
    case SerializedNodeKind::String:
        node.flags = node.flags | SerializedNodeFlags::Align4After;
        break;
    default:
        break;
    }
}

SerializedNodeId AddTypeNode(
    SerializedTypeTree& tree,
    const meta::Type& type,
    const std::string& name,
    const SerializedNodeId parent)
{
    SerializedTypeTreeNode node;
    node.parent = parent;
    node.serializedName = name;
    node.fieldId = StableFieldId(type, name);
    SetNodeLayout(node, type);
    if (!type.IsArray() && node.kind == SerializedNodeKind::Generic && type.IsClass() &&
        !IsObjectReferenceType(type) && !type.IsEnum() && type.GetFields().empty())
    {
        node.kind = SerializedNodeKind::Opaque;
        node.flags = node.flags | SerializedNodeFlags::Unsupported |
            SerializedNodeFlags::ReadOnly | SerializedNodeFlags::DebugOnly;
    }
    if (type.IsArray())
    {
        node.kind = SerializedNodeKind::Generic;
        node.flags = node.flags | SerializedNodeFlags::Array | SerializedNodeFlags::Align4After;
    }
    const auto id = tree.AddNode(std::move(node));

    if (type.IsArray())
    {
        AddTypeNode(tree, type.GetArrayType(), "data", id);
    }
    else if ((node.kind == SerializedNodeKind::Generic ||
              node.kind == SerializedNodeKind::AnimationCurve ||
              node.kind == SerializedNodeKind::Gradient ||
              node.kind == SerializedNodeKind::ExposedReference) &&
             type.IsClass() && !IsObjectReferenceType(type) && !type.IsEnum())
    {
        for (const auto& field : type.GetFields())
            AddTypeNode(tree, field.GetType(), field.GetName(), id);
    }
    return id;
}

const SerializedTypeTreeNode* FirstChild(const SerializedTypeTree& tree, const SerializedTypeTreeNode& node)
{
    return node.firstChild == 0u ? nullptr : tree.FindNode(node.firstChild);
}

std::vector<const SerializedTypeTreeNode*> Children(const SerializedTypeTree& tree, const SerializedTypeTreeNode& node)
{
    std::vector<const SerializedTypeTreeNode*> result;
    for (auto child = FirstChild(tree, node); child != nullptr; child = tree.FindNode(child->nextSibling))
        result.push_back(child);
    return result;
}

bool ReadU32At(const std::vector<uint8_t>& bytes, const size_t offset, uint32_t& value)
{
    if (offset > bytes.size() || bytes.size() - offset < sizeof(uint32_t))
        return false;
    BinaryBufferReader reader(bytes.data() + offset, bytes.size() - offset);
    return reader.ReadU32(value);
}

size_t Align4Size(const size_t size)
{
    return (size + 3u) & ~size_t {3u};
}

std::optional<size_t> CalculateNodeSize(
    const SerializedTypeTree& tree,
    const SerializedTypeTreeNode& node,
    const std::vector<uint8_t>& bytes,
    const size_t offset)
{
    if (offset > bytes.size())
        return std::nullopt;

    size_t size = 0u;
    if (HasSerializedNodeFlag(node.flags, SerializedNodeFlags::Array))
    {
        uint32_t count = 0u;
        if (!ReadU32At(bytes, offset, count))
            return std::nullopt;
        size = sizeof(uint32_t);
        const auto* element = FirstChild(tree, node);
        if (element == nullptr)
            return std::nullopt;
        for (uint32_t index = 0u; index < count; ++index)
        {
            if (size > bytes.size() - offset)
                return std::nullopt;
            const auto elementSize = CalculateNodeSize(tree, *element, bytes, offset + size);
            if (!elementSize.has_value())
                return std::nullopt;
            if (*elementSize > std::numeric_limits<size_t>::max() - size)
                return std::nullopt;
            size += *elementSize;
        }
    }
    else if (node.kind == SerializedNodeKind::String)
    {
        uint32_t length = 0u;
        if (!ReadU32At(bytes, offset, length))
            return std::nullopt;
        if (static_cast<size_t>(length) > std::numeric_limits<size_t>::max() - sizeof(uint32_t))
            return std::nullopt;
        size = sizeof(uint32_t) + static_cast<size_t>(length);
    }
    else if (node.fixedByteSize >= 0)
    {
        size = static_cast<size_t>(node.fixedByteSize);
    }
    else
    {
        const auto children = Children(tree, node);
        if (children.empty())
        {
            uint32_t length = 0u;
            if (!ReadU32At(bytes, offset, length))
                return std::nullopt;
            if (static_cast<size_t>(length) > std::numeric_limits<size_t>::max() - sizeof(uint32_t))
                return std::nullopt;
            size = sizeof(uint32_t) + static_cast<size_t>(length);
        }
        else
        {
            for (const auto* child : children)
            {
                if (size > bytes.size() - offset)
                    return std::nullopt;
                const auto childSize = CalculateNodeSize(tree, *child, bytes, offset + size);
                if (!childSize.has_value() || *childSize > std::numeric_limits<size_t>::max() - size)
                    return std::nullopt;
                size += *childSize;
            }
        }
    }

    if (HasSerializedNodeFlag(node.flags, SerializedNodeFlags::Align4After))
    {
        if (size > std::numeric_limits<size_t>::max() - offset ||
            offset + size > std::numeric_limits<size_t>::max() - 3u)
        {
            return std::nullopt;
        }
        // Alignment is relative to the byte stream, not the node's local
        // payload. A string nested after a byte-sized sibling must advance to
        // the next absolute 4-byte boundary before its following sibling.
        size = Align4Size(offset + size) - offset;
    }
    if (size > bytes.size() - offset)
        return std::nullopt;
    return size;
}

void CollectReferenceRelocations(
    const SerializedTypeTree& tree,
    const SerializedTypeTreeNode& node,
    const std::vector<uint8_t>& bytes,
    const size_t offset,
    const size_t sliceOffset,
    const size_t sliceEnd,
    std::vector<SerializedByteSlice::ReferenceRelocation>& output)
{
    if (offset > bytes.size() || offset >= sliceEnd)
        return;
    if (node.kind == SerializedNodeKind::ObjectReference && node.fixedByteSize == 4)
    {
        uint32_t slot = 0u;
        if (ReadU32At(bytes, offset, slot) && slot != 0u && offset >= sliceOffset &&
            sizeof(uint32_t) <= sliceEnd - offset)
        {
            const auto relative = offset - sliceOffset;
            if (relative <= std::numeric_limits<uint32_t>::max())
                output.push_back({static_cast<uint32_t>(relative), slot});
        }
        return;
    }
    if (HasSerializedNodeFlag(node.flags, SerializedNodeFlags::Array))
    {
        uint32_t count = 0u;
        if (!ReadU32At(bytes, offset, count))
            return;
        const auto* element = FirstChild(tree, node);
        if (element == nullptr)
            return;
        if (offset > std::numeric_limits<size_t>::max() - sizeof(uint32_t))
            return;
        size_t elementOffset = offset + sizeof(uint32_t);
        for (uint32_t index = 0u; index < count; ++index)
        {
            CollectReferenceRelocations(tree, *element, bytes, elementOffset, sliceOffset, sliceEnd, output);
            const auto elementSize = CalculateNodeSize(tree, *element, bytes, elementOffset);
            if (!elementSize.has_value() || elementOffset > bytes.size() || *elementSize > bytes.size() - elementOffset)
                return;
            if (*elementSize > sliceEnd - std::min(elementOffset, sliceEnd))
                return;
            if (*elementSize > std::numeric_limits<size_t>::max() - elementOffset)
                return;
            elementOffset += *elementSize;
        }
        return;
    }
    if (node.kind == SerializedNodeKind::String)
        return;
    size_t childOffset = offset;
    for (const auto* child : Children(tree, node))
    {
        CollectReferenceRelocations(tree, *child, bytes, childOffset, sliceOffset, sliceEnd, output);
        const auto childSize = CalculateNodeSize(tree, *child, bytes, childOffset);
        if (!childSize.has_value() || childOffset > bytes.size() || *childSize > bytes.size() - childOffset)
            return;
        if (*childSize > sliceEnd - std::min(childOffset, sliceEnd))
            return;
        if (*childSize > std::numeric_limits<size_t>::max() - childOffset)
            return;
        childOffset += *childSize;
    }
}

bool WriteU32At(std::vector<uint8_t>& bytes, const size_t offset, const uint32_t value)
{
    if (offset > bytes.size() || bytes.size() - offset < sizeof(uint32_t))
        return false;
    BinaryBufferWriter writer;
    writer.WriteU32(value);
    std::memcpy(bytes.data() + offset, writer.bytes.data(), sizeof(uint32_t));
    return true;
}

bool RelocateReferenceSlotsNode(
    std::vector<uint8_t>& bytes,
    const SerializedTypeTree& tree,
    const SerializedTypeTreeNode& node,
    const size_t offset,
    const SerializedReferenceTable& sourceReferences,
    SerializedReferenceTable& destinationReferences,
    std::string* error)
{
    if (offset > bytes.size())
    {
        if (error != nullptr) *error = "Serialized reference relocation offset is outside the byte stream.";
        return false;
    }
    if (node.kind == SerializedNodeKind::ObjectReference && node.fixedByteSize == 4)
    {
        uint32_t slot = 0u;
        if (!ReadU32At(bytes, offset, slot))
        {
            if (error != nullptr) *error = "Serialized reference slot is truncated.";
            return false;
        }
        if (slot == 0u)
            return true;
        const auto* entry = sourceReferences.Find(slot);
        if (entry == nullptr)
        {
            if (error != nullptr) *error = "Serialized reference slot is not present in the source table.";
            return false;
        }
        const auto relocated = destinationReferences.Intern(*entry);
        return WriteU32At(bytes, offset, relocated);
    }
    if (HasSerializedNodeFlag(node.flags, SerializedNodeFlags::Array))
    {
        uint32_t count = 0u;
        if (!ReadU32At(bytes, offset, count))
        {
            if (error != nullptr) *error = "Serialized array length is truncated during reference relocation.";
            return false;
        }
        const auto* element = FirstChild(tree, node);
        if (element == nullptr)
        {
            if (error != nullptr) *error = "Serialized array has no element schema during reference relocation.";
            return false;
        }
        if (offset > std::numeric_limits<size_t>::max() - sizeof(uint32_t))
        {
            if (error != nullptr) *error = "Serialized array offset overflow during reference relocation.";
            return false;
        }
        size_t elementOffset = offset + sizeof(uint32_t);
        for (uint32_t index = 0u; index < count; ++index)
        {
            if (!RelocateReferenceSlotsNode(
                    bytes,
                    tree,
                    *element,
                    elementOffset,
                    sourceReferences,
                    destinationReferences,
                    error))
                return false;
            const auto elementSize = CalculateNodeSize(tree, *element, bytes, elementOffset);
            if (!elementSize.has_value() || elementOffset > bytes.size() || *elementSize > bytes.size() - elementOffset)
            {
                if (error != nullptr) *error = "Serialized array element is truncated during reference relocation.";
                return false;
            }
            if (*elementSize > std::numeric_limits<size_t>::max() - elementOffset)
            {
                if (error != nullptr) *error = "Serialized array element offset overflow during reference relocation.";
                return false;
            }
            elementOffset += *elementSize;
        }
        return true;
    }
    if (node.kind == SerializedNodeKind::String)
        return true;

    size_t childOffset = offset;
    for (const auto* child : Children(tree, node))
    {
        if (!RelocateReferenceSlotsNode(
                bytes,
                tree,
                *child,
                childOffset,
                sourceReferences,
                destinationReferences,
                error))
            return false;
        const auto childSize = CalculateNodeSize(tree, *child, bytes, childOffset);
        if (!childSize.has_value() || childOffset > bytes.size() || *childSize > bytes.size() - childOffset)
        {
            if (error != nullptr) *error = "Serialized child is truncated during reference relocation.";
            return false;
        }
        if (*childSize > std::numeric_limits<size_t>::max() - childOffset)
        {
            if (error != nullptr) *error = "Serialized child offset overflow during reference relocation.";
            return false;
        }
        childOffset += *childSize;
    }
    return true;
}

struct ArrayPath
{
    std::string parent;
    size_t index = 0u;
};

std::optional<ArrayPath> ParseArrayPath(const std::string_view path)
{
    const auto marker = path.rfind(".Array.data[");
    const auto plain = path.rfind("[");
    const auto start = marker != std::string_view::npos ? marker + std::string_view(".Array.data[").size() : plain==std::string_view::npos?plain:plain+1u;
    if (start == std::string_view::npos || path.empty() || path.back() != ']')
        return std::nullopt;
    const auto digits = path.substr(start, path.size() - start - 1u);
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string_view::npos)
        return std::nullopt;
    size_t index = 0u;
    try
    {
        index = static_cast<size_t>(std::stoull(std::string(digits)));
    }
    catch (...)
    {
        return std::nullopt;
    }
    const auto parentEnd = marker != std::string_view::npos ? marker : plain;
    return ArrayPath {std::string(path.substr(0u, parentEnd)), index};
}

bool WriteVariantNode(
    const meta::Variant& value,
    const SerializedTypeTree& tree,
    const SerializedTypeTreeNode& node,
    SerializedReferenceTable& references,
    BinaryBufferWriter& writer,
    std::string* error);

bool WriteScalar(
    const meta::Variant& value,
    const SerializedTypeTreeNode& node,
    SerializedReferenceTable& references,
    BinaryBufferWriter& writer,
    std::string* error)
{
    switch (node.kind)
    {
    case SerializedNodeKind::Boolean:
        writer.WriteBool(value.ToBool());
        return true;
    case SerializedNodeKind::Integer:
    case SerializedNodeKind::Enum:
        if (node.kind == SerializedNodeKind::Enum)
        {
            const auto numeric = value.GetType().GetEnum().GetBits(value);
            switch (node.numericKind)
            {
            case SerializedNumericKind::Int8: writer.WriteI8(static_cast<int8_t>(numeric)); return true;
            case SerializedNumericKind::UInt8: writer.WriteU8(static_cast<uint8_t>(numeric)); return true;
            case SerializedNumericKind::Int16: writer.WriteI16(static_cast<int16_t>(numeric)); return true;
            case SerializedNumericKind::UInt16: writer.WriteU16(static_cast<uint16_t>(numeric)); return true;
            case SerializedNumericKind::UInt32: writer.WriteU32(static_cast<uint32_t>(numeric)); return true;
            case SerializedNumericKind::Int64: writer.WriteI64(static_cast<int64_t>(numeric)); return true;
            case SerializedNumericKind::UInt64: writer.WriteU64(static_cast<uint64_t>(numeric)); return true;
            default: writer.WriteI32(static_cast<int32_t>(numeric)); return true;
            }
        }
        switch (node.numericKind)
        {
        case SerializedNumericKind::Int8: writer.WriteI8(value.GetValue<int8_t>()); return true;
        case SerializedNumericKind::UInt8: writer.WriteU8(value.GetValue<uint8_t>()); return true;
        case SerializedNumericKind::Int16: writer.WriteI16(value.GetValue<int16_t>()); return true;
        case SerializedNumericKind::UInt16: writer.WriteU16(value.GetValue<uint16_t>()); return true;
        case SerializedNumericKind::UInt32: writer.WriteU32(value.GetValue<uint32_t>()); return true;
        case SerializedNumericKind::Int64: writer.WriteI64(value.GetValue<int64_t>()); return true;
        case SerializedNumericKind::UInt64: writer.WriteU64(value.GetValue<uint64_t>()); return true;
        case SerializedNumericKind::Character: writer.WriteU32(static_cast<uint32_t>(value.ToInt())); return true;
        case SerializedNumericKind::Int32:
        default: writer.WriteI32(value.GetValue<int32_t>()); return true;
        }
    case SerializedNodeKind::Float:
        if (node.numericKind == SerializedNumericKind::Float64)
            writer.WriteDouble(value.GetValue<double>());
        else
            writer.WriteFloat(value.GetValue<float>());
        return true;
    case SerializedNodeKind::String:
        writer.WriteAlignedString(value.ToString());
        return true;
    case SerializedNodeKind::Vector2:
    {
        const auto vector = value.GetValue<Maths::Vector2>();
        writer.WriteFloat(vector.x); writer.WriteFloat(vector.y);
        return true;
    }
    case SerializedNodeKind::Vector3:
    {
        const auto vector = value.GetValue<Maths::Vector3>();
        writer.WriteFloat(vector.x); writer.WriteFloat(vector.y); writer.WriteFloat(vector.z);
        return true;
    }
    case SerializedNodeKind::Vector4:
    case SerializedNodeKind::Quaternion:
    {
        const auto vector = node.kind == SerializedNodeKind::Quaternion
            ? Maths::Vector4(value.GetValue<Maths::Quaternion>().x,
                value.GetValue<Maths::Quaternion>().y,
                value.GetValue<Maths::Quaternion>().z,
                value.GetValue<Maths::Quaternion>().w)
            : value.GetValue<Maths::Vector4>();
        writer.WriteFloat(vector.x); writer.WriteFloat(vector.y);
        writer.WriteFloat(vector.z); writer.WriteFloat(vector.w);
        return true;
    }
    case SerializedNodeKind::Color:
    {
        const auto color = value.GetValue<Maths::Color>();
        writer.WriteFloat(color.r); writer.WriteFloat(color.g); writer.WriteFloat(color.b); writer.WriteFloat(color.a);
        return true;
    }
    case SerializedNodeKind::Rect:
    {
        const auto rect = value.GetValue<Maths::Rect>();
        writer.WriteFloat(rect.x); writer.WriteFloat(rect.y); writer.WriteFloat(rect.width); writer.WriteFloat(rect.height);
        return true;
    }
    case SerializedNodeKind::Vector2Int:
    {
        const auto vector = value.GetValue<Maths::Vector2Int>();
        writer.WriteI32(vector.x); writer.WriteI32(vector.y);
        return true;
    }
    case SerializedNodeKind::Vector3Int:
    {
        const auto vector = value.GetValue<Maths::Vector3Int>();
        writer.WriteI32(vector.x); writer.WriteI32(vector.y); writer.WriteI32(vector.z);
        return true;
    }
    case SerializedNodeKind::RectInt:
    {
        const auto rect = value.GetValue<Maths::RectInt>();
        writer.WriteI32(rect.x); writer.WriteI32(rect.y); writer.WriteI32(rect.width); writer.WriteI32(rect.height);
        return true;
    }
    case SerializedNodeKind::Bounds:
    {
        const auto bounds = value.GetValue<Maths::Bounds>();
        writer.WriteFloat(bounds.center.x); writer.WriteFloat(bounds.center.y); writer.WriteFloat(bounds.center.z);
        writer.WriteFloat(bounds.size.x); writer.WriteFloat(bounds.size.y); writer.WriteFloat(bounds.size.z);
        return true;
    }
    case SerializedNodeKind::BoundsInt:
    {
        const auto bounds = value.GetValue<Maths::BoundsInt>();
        writer.WriteI32(bounds.position.x); writer.WriteI32(bounds.position.y); writer.WriteI32(bounds.position.z);
        writer.WriteI32(bounds.size.x); writer.WriteI32(bounds.size.y); writer.WriteI32(bounds.size.z);
        return true;
    }
    case SerializedNodeKind::ObjectReference:
    {
        uint32_t slot = 0u;
        SerializedReferenceEntry entry;
        entry.expectedTypeName = node.typeName;
        if (value.GetType().IsPointer())
        {
            auto* object = value.GetValue<Object*>();
            entry.isNull = object == nullptr;
            if (object != nullptr)
            {
                entry.resourcePath = object->GetType().GetName();
                if (!PersistentManager::Instance().InstanceIDToObjectIdentifier(object->GetInstanceID(), entry.identifier))
                    entry.identifier = PersistentManager::Instance().GetOrCreateLocalObjectIdentifier(*object);
            }
            slot = references.Intern(std::move(entry));
        }
        else if (const auto* handler = Internal::FindPPtrObjectGraphHandler(value.GetType());
            handler != nullptr && handler->serializeValue != nullptr)
        {
            try
            {
                const auto serialized = handler->serializeValue(value);
                if (serialized.GetKind() != PropertyValue::Kind::ObjectReference)
                {
                    if (error != nullptr) *error = "PPtr codec returned an incompatible reference value.";
                    return false;
                }
                entry.identifier = serialized.GetObjectReference();
                entry.resourcePath = entry.identifier.filePath;
                entry.isNull = !entry.identifier.IsValid();
                slot = references.Intern(std::move(entry));
            }
            catch (const std::exception& exception)
            {
                if (error != nullptr) *error = exception.what();
                return false;
            }
        }
        else if (value.GetType() == NLS_TYPEOF(PPtr<GameObject>))
        {
            const auto& reference = value.GetValue<PPtr<GameObject>>();
            entry.isNull = reference.IsNull();
            if (!entry.isNull)
            {
                if (!PersistentManager::Instance().InstanceIDToObjectIdentifier(reference.GetInstanceID(), entry.identifier))
                {
                    if (auto* object = Object::IDToPointer(reference.GetInstanceID()); object != nullptr)
                        entry.identifier = PersistentManager::Instance().GetOrCreateLocalObjectIdentifier(*object);
                }
                if (!entry.identifier.IsValid())
                {
                    if (error != nullptr) *error = "Non-null GameObject PPtr has no stable identifier.";
                    return false;
                }
            }
            slot = references.Intern(std::move(entry));
        }
        else if (value.GetType() == NLS_TYPEOF(PPtr<Components::Component>))
        {
            const auto& reference = value.GetValue<PPtr<Components::Component>>();
            entry.isNull = reference.IsNull();
            if (!entry.isNull)
            {
                if (!PersistentManager::Instance().InstanceIDToObjectIdentifier(reference.GetInstanceID(), entry.identifier))
                {
                    if (auto* object = Object::IDToPointer(reference.GetInstanceID()); object != nullptr)
                        entry.identifier = PersistentManager::Instance().GetOrCreateLocalObjectIdentifier(*object);
                }
                if (!entry.identifier.IsValid())
                {
                    if (error != nullptr) *error = "Non-null Component PPtr has no stable identifier.";
                    return false;
                }
            }
            slot = references.Intern(std::move(entry));
        }
        else
        {
            if (error != nullptr) *error = "Unsupported object reference type: " + value.GetType().GetName();
            return false;
        }
        writer.WriteU32(slot);
        return true;
    }
    default:
        if (error != nullptr)
            *error = "Unsupported scalar serialized node kind.";
        return false;
    }
}

bool WriteVariantNode(
    const meta::Variant& value,
    const SerializedTypeTree& tree,
    const SerializedTypeTreeNode& node,
    SerializedReferenceTable& references,
    BinaryBufferWriter& writer,
    std::string* error)
{
    if (HasSerializedNodeFlag(node.flags, SerializedNodeFlags::Array))
    {
        if (!value.IsArray())
        {
            if (error != nullptr) *error = "Serialized array value has an incompatible Variant type.";
            return false;
        }
        const auto array = value.GetArray();
        if (array.Size() > std::numeric_limits<uint32_t>::max())
        {
            if (error != nullptr) *error = "Serialized array is too large.";
            return false;
        }
        writer.WriteU32(static_cast<uint32_t>(array.Size()));
        const auto* element = FirstChild(tree, node);
        if (element == nullptr)
        {
            if (error != nullptr) *error = "Serialized array has no element schema.";
            return false;
        }
        for (size_t index = 0u; index < array.Size(); ++index)
        {
            if (!WriteVariantNode(array.GetValue(index), tree, *element, references, writer, error))
                return false;
        }
        writer.Align4();
        return true;
    }

    const bool structured = node.kind == SerializedNodeKind::Generic ||
        node.kind == SerializedNodeKind::AnimationCurve ||
        node.kind == SerializedNodeKind::Gradient ||
        node.kind == SerializedNodeKind::ExposedReference;
    if (!structured)
        return WriteScalar(value, node, references, writer, error);

    const auto children = Children(tree, node);
    if (children.empty())
    {
        if (error != nullptr)
            *error = "Serialized generic value has no registered field schema: " + node.typeName;
        return false;
    }
    const auto type = value.GetType();
    for (const auto* child : children)
    {
        const auto& field = type.GetField(child->serializedName);
        if (!field.IsValid())
        {
            if (error != nullptr) *error = "Serialized field is missing from the Variant type: " + child->serializedName;
            return false;
        }
        if (!WriteVariantNode(field.GetValue(value), tree, *child, references, writer, error))
            return false;
    }
    return true;
}

std::optional<meta::Variant> ReadVariantNode(
    BinaryBufferReader& reader,
    const SerializedTypeTree& tree,
    const SerializedTypeTreeNode& node,
    const meta::Type& type,
    const SerializedReferenceTable& references,
    std::string* error);

std::optional<meta::Variant> NormalizeDecodedEnum(
    const meta::Variant& raw,
    const SerializedTypeTreeNode& node,
    const meta::Type& type)
{
    if (node.kind != SerializedNodeKind::Enum || !type.IsEnum())
        return raw;
    uint64_t bits = 0;
    switch(node.numericKind)
    {
    case SerializedNumericKind::Int8: bits=static_cast<uint64_t>(raw.GetValue<int8_t>());break;
    case SerializedNumericKind::UInt8: bits=raw.GetValue<uint8_t>();break;
    case SerializedNumericKind::Int16: bits=static_cast<uint64_t>(raw.GetValue<int16_t>());break;
    case SerializedNumericKind::UInt16: bits=raw.GetValue<uint16_t>();break;
    case SerializedNumericKind::UInt32: bits=raw.GetValue<uint32_t>();break;
    case SerializedNumericKind::Int64: bits=static_cast<uint64_t>(raw.GetValue<int64_t>());break;
    case SerializedNumericKind::UInt64: bits=raw.GetValue<uint64_t>();break;
    default: bits=static_cast<uint64_t>(raw.GetValue<int32_t>());break;
    }
    return type.GetEnum().FromBits(bits);
}

std::optional<meta::Variant> ReadScalar(
    BinaryBufferReader& reader,
    const SerializedTypeTreeNode& node,
    const meta::Type& type,
    const SerializedReferenceTable& references,
    std::string* error)
{
    switch (node.kind)
    {
    case SerializedNodeKind::Boolean:
    {
        bool value = false;
        return reader.ReadBool(value) ? std::optional<meta::Variant>(meta::Variant(value)) : std::nullopt;
    }
    case SerializedNodeKind::Integer:
    case SerializedNodeKind::Enum:
    {
        switch (node.numericKind)
        {
        case SerializedNumericKind::Int8: { int8_t value; if (reader.ReadI8(value)) return NormalizeDecodedEnum(meta::Variant(value), node, type); break; }
        case SerializedNumericKind::UInt8: { uint8_t value; if (reader.ReadU8(value)) return NormalizeDecodedEnum(meta::Variant(value), node, type); break; }
        case SerializedNumericKind::Int16: { int16_t value; if (reader.ReadI16(value)) return NormalizeDecodedEnum(meta::Variant(value), node, type); break; }
        case SerializedNumericKind::UInt16: { uint16_t value; if (reader.ReadU16(value)) return NormalizeDecodedEnum(meta::Variant(value), node, type); break; }
        case SerializedNumericKind::UInt32: { uint32_t value; if (reader.ReadU32(value)) return NormalizeDecodedEnum(meta::Variant(value), node, type); break; }
        case SerializedNumericKind::Int64: { int64_t value; if (reader.ReadI64(value)) return NormalizeDecodedEnum(meta::Variant(value), node, type); break; }
        case SerializedNumericKind::UInt64: { uint64_t value; if (reader.ReadU64(value)) return NormalizeDecodedEnum(meta::Variant(value), node, type); break; }
        case SerializedNumericKind::Character:
        case SerializedNumericKind::Int32:
        default: { int32_t value; if (reader.ReadI32(value)) return NormalizeDecodedEnum(meta::Variant(value), node, type); break; }
        }
        break;
    }
    case SerializedNodeKind::Float:
        if (node.numericKind == SerializedNumericKind::Float64)
        {
            double value = 0.0;
            if (reader.ReadDouble(value)) return meta::Variant(value);
        }
        else
        {
            float value = 0.0f;
            if (reader.ReadFloat(value)) return meta::Variant(value);
        }
        break;
    case SerializedNodeKind::String:
    {
        std::string value;
        if (reader.ReadAlignedString(value)) return meta::Variant(std::move(value));
        break;
    }
    case SerializedNodeKind::Vector2:
    {
        float x, y;
        if (reader.ReadFloat(x) && reader.ReadFloat(y)) return meta::Variant(Maths::Vector2(x, y));
        break;
    }
    case SerializedNodeKind::Vector3:
    {
        float x, y, z;
        if (reader.ReadFloat(x) && reader.ReadFloat(y) && reader.ReadFloat(z)) return meta::Variant(Maths::Vector3(x, y, z));
        break;
    }
    case SerializedNodeKind::Vector4:
    {
        float x, y, z, w;
        if (reader.ReadFloat(x) && reader.ReadFloat(y) && reader.ReadFloat(z) && reader.ReadFloat(w)) return meta::Variant(Maths::Vector4(x, y, z, w));
        break;
    }
    case SerializedNodeKind::Quaternion:
    {
        float x, y, z, w;
        if (reader.ReadFloat(x) && reader.ReadFloat(y) && reader.ReadFloat(z) && reader.ReadFloat(w)) return meta::Variant(Maths::Quaternion(x, y, z, w));
        break;
    }
    case SerializedNodeKind::Color:
    {
        float r, g, b, a;
        if (reader.ReadFloat(r) && reader.ReadFloat(g) && reader.ReadFloat(b) && reader.ReadFloat(a)) return meta::Variant(Maths::Color(r, g, b, a));
        break;
    }
    case SerializedNodeKind::Rect:
    {
        float x, y, width, height;
        if (reader.ReadFloat(x) && reader.ReadFloat(y) && reader.ReadFloat(width) && reader.ReadFloat(height)) return meta::Variant(Maths::Rect(x, y, width, height));
        break;
    }
    case SerializedNodeKind::Vector2Int:
    {
        int32_t x, y;
        if (reader.ReadI32(x) && reader.ReadI32(y)) return meta::Variant(Maths::Vector2Int(x, y));
        break;
    }
    case SerializedNodeKind::Vector3Int:
    {
        int32_t x, y, z;
        if (reader.ReadI32(x) && reader.ReadI32(y) && reader.ReadI32(z)) return meta::Variant(Maths::Vector3Int(x, y, z));
        break;
    }
    case SerializedNodeKind::RectInt:
    {
        int32_t x, y, width, height;
        if (reader.ReadI32(x) && reader.ReadI32(y) && reader.ReadI32(width) && reader.ReadI32(height)) return meta::Variant(Maths::RectInt(x, y, width, height));
        break;
    }
    case SerializedNodeKind::Bounds:
    {
        float cx, cy, cz, sx, sy, sz;
        if (reader.ReadFloat(cx) && reader.ReadFloat(cy) && reader.ReadFloat(cz) && reader.ReadFloat(sx) && reader.ReadFloat(sy) && reader.ReadFloat(sz))
            return meta::Variant(Maths::Bounds {Maths::Vector3(cx, cy, cz), Maths::Vector3(sx, sy, sz)});
        break;
    }
    case SerializedNodeKind::BoundsInt:
    {
        int32_t px, py, pz, sx, sy, sz;
        if (reader.ReadI32(px) && reader.ReadI32(py) && reader.ReadI32(pz) && reader.ReadI32(sx) && reader.ReadI32(sy) && reader.ReadI32(sz))
            return meta::Variant(Maths::BoundsInt {Maths::Vector3Int(px, py, pz), Maths::Vector3Int(sx, sy, sz)});
        break;
    }
    case SerializedNodeKind::ObjectReference:
    {
        uint32_t slot = 0u;
        if (reader.ReadU32(slot))
        {
            const auto* entry = slot == 0u ? nullptr : references.Find(slot);
            if (slot == 0u || entry != nullptr)
            {
                if (entry == nullptr || entry->isNull)
                {
                    if (type == NLS_TYPEOF(GameObject*))
                        return meta::Variant(static_cast<GameObject*>(nullptr), meta::variant_policy::WrapObject {});
                    if (type == NLS_TYPEOF(Components::Component*))
                        return meta::Variant(static_cast<Components::Component*>(nullptr), meta::variant_policy::WrapObject {});
#define NLS_SERIALIZED_NULL_RESOURCE_POINTER(targetType, label, artifactType, subAssetPrefix) \
                    if (type == NLS_TYPEOF(targetType*)) \
                        return meta::Variant(static_cast<targetType*>(nullptr), meta::variant_policy::WrapObject {}); \
                    if (type == NLS_TYPEOF(PPtr<targetType>)) \
                        return meta::Variant(PPtr<targetType> {});
                    NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_SERIALIZED_NULL_RESOURCE_POINTER)
#undef NLS_SERIALIZED_NULL_RESOURCE_POINTER
                    if (type == NLS_TYPEOF(PPtr<GameObject>))
                        return meta::Variant(PPtr<GameObject> {});
                    if (type == NLS_TYPEOF(PPtr<Components::Component>))
                        return meta::Variant(PPtr<Components::Component> {});
                    return meta::Variant();
                }
                if (const auto* handler = Internal::FindPPtrObjectGraphHandler(type);
                    handler != nullptr && handler->deserializeValue != nullptr)
                {
                    if (const auto decoded = handler->deserializeValue(entry->identifier); decoded.has_value())
                        return decoded;
                    if (error != nullptr) *error = "PPtr reference could not be resolved for type '" + type.GetName() + "'.";
                    return std::nullopt;
                }
                const auto instanceId = PersistentManager::Instance().ObjectIdentifierToInstanceID(entry->identifier);
                if (type == NLS_TYPEOF(PPtr<GameObject>))
                    return meta::Variant(PPtr<GameObject>(instanceId));
                if (type == NLS_TYPEOF(PPtr<Components::Component>))
                    return meta::Variant(PPtr<Components::Component>(instanceId));
                if (!type.IsPointer())
                {
                    if (error != nullptr) *error = "Unsupported object reference type '" + type.GetName() + "'.";
                    return std::nullopt;
                }
                auto* object = instanceId == InstanceID_None ? nullptr : Object::IDToPointer(instanceId);
#define NLS_SERIALIZED_TYPED_RESOURCE_POINTER(targetType, label, artifactType, subAssetPrefix) \
                if (type == NLS_TYPEOF(targetType*)) \
                    return meta::Variant(dynamic_cast<targetType*>(object), meta::variant_policy::WrapObject {}); \
                if (type == NLS_TYPEOF(PPtr<targetType>)) \
                    return meta::Variant(PPtr<targetType>(instanceId));
                NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_SERIALIZED_TYPED_RESOURCE_POINTER)
#undef NLS_SERIALIZED_TYPED_RESOURCE_POINTER
                if (type == NLS_TYPEOF(GameObject*))
                    return meta::Variant(dynamic_cast<GameObject*>(object), meta::variant_policy::WrapObject {});
                if (type == NLS_TYPEOF(Components::Component*))
                    return meta::Variant(dynamic_cast<Components::Component*>(object), meta::variant_policy::WrapObject {});
                return meta::Variant(object, meta::variant_policy::WrapObject {});
            }
        }
        break;
    }
    default:
        break;
    }
    if (error != nullptr)
        *error = "Truncated or unsupported serialized scalar value for type '" + type.GetName() + "'.";
    return std::nullopt;
}

std::optional<meta::Variant> ReadVariantNode(
    BinaryBufferReader& reader,
    const SerializedTypeTree& tree,
    const SerializedTypeTreeNode& node,
    const meta::Type& type,
    const SerializedReferenceTable& references,
    std::string* error)
{
    if (HasSerializedNodeFlag(node.flags, SerializedNodeFlags::Array))
    {
        uint32_t count = 0u;
        if (!reader.ReadU32(count))
        {
            if (error != nullptr) *error = "Truncated serialized array length.";
            return std::nullopt;
        }
        const auto& constructor = type.GetArrayConstructor();
        if (!constructor.IsValid())
        {
            if (error != nullptr) *error = "Serialized array type has no reflected array constructor.";
            return std::nullopt;
        }
        auto result = constructor.Invoke();
        auto array = result.GetArray();
        const auto* element = FirstChild(tree, node);
        if (element == nullptr)
            return std::nullopt;
        for (uint32_t index = 0u; index < count; ++index)
        {
            auto value = ReadVariantNode(reader, tree, *element, type.GetArrayType(), references, error);
            if (!value.has_value() || !value->IsValid())
            {
                if (error != nullptr && value.has_value())
                    *error = "Serialized array element decoded to an invalid Variant for type '" + type.GetArrayType().GetName() + "'.";
                return std::nullopt;
            }
            array.Insert(index, meta::Argument(*value));
        }
        if (!reader.Align4())
            return std::nullopt;
        return result;
    }

    const bool structured = node.kind == SerializedNodeKind::Generic ||
        node.kind == SerializedNodeKind::AnimationCurve ||
        node.kind == SerializedNodeKind::Gradient ||
        node.kind == SerializedNodeKind::ExposedReference;
    if (!structured)
        return ReadScalar(reader, node, type, references, error);

    const auto children = Children(tree, node);
    if (children.empty())
    {
        if (error != nullptr)
            *error = "Serialized generic value has no registered field schema: " + node.typeName;
        return std::nullopt;
    }

    const auto& constructor = type.GetConstructor();
    if (!constructor.IsValid())
    {
        if (error != nullptr) *error = "Serialized struct type has no reflected default constructor: " + type.GetName();
        return std::nullopt;
    }
    auto result = constructor.Invoke();
    for (const auto* child : children)
    {
        const auto& field = type.GetField(child->serializedName);
        if (!field.IsValid())
            return std::nullopt;
        auto value = ReadVariantNode(reader, tree, *child, field.GetType(), references, error);
        if (!value.has_value() || !field.SetValue(result, *value))
            return std::nullopt;
    }
    return result;
}
} // namespace

SerializedNodeId SerializedTypeTree::AddNode(SerializedTypeTreeNode node)
{
    if (node.id == 0u)
        node.id = static_cast<SerializedNodeId>(m_nodes.size() + 1u);
    if (node.id != static_cast<SerializedNodeId>(m_nodes.size() + 1u))
        node.id = static_cast<SerializedNodeId>(m_nodes.size() + 1u);
    if (node.parent == 0u && m_root == 0u)
        m_root = node.id;
    m_nodes.push_back(std::move(node));
    return m_nodes.back().id;
}

void SerializedTypeTree::RebuildIndexes()
{
    m_pathIndex.clear();
    m_fieldIndex.clear();
    m_paths.clear();
    m_root = 0u;

    for (size_t index = 0u; index < m_nodes.size(); ++index)
    {
        m_nodes[index].id = static_cast<SerializedNodeId>(index + 1u);
        m_nodes[index].firstChild = 0u;
        m_nodes[index].nextSibling = 0u;
        if (m_nodes[index].parent == 0u && m_root == 0u)
            m_root = m_nodes[index].id;
    }

    std::unordered_map<SerializedNodeId, SerializedNodeId> lastChild;
    for (const auto& node : m_nodes)
    {
        if (node.parent == 0u)
            continue;
        auto* parent = const_cast<SerializedTypeTreeNode*>(FindNode(node.parent));
        if (parent == nullptr)
            continue;
        const auto previous = lastChild[node.parent];
        if (previous == 0u)
            parent->firstChild = node.id;
        else if (auto* previousNode = const_cast<SerializedTypeTreeNode*>(FindNode(previous)); previousNode != nullptr)
            previousNode->nextSibling = node.id;
        lastChild[node.parent] = node.id;
    }

    if (m_root != 0u)
        RebuildNodePath(m_root, {});
    for (auto& node : m_nodes)
    {
        if (node.fieldId == 0u)
        {
            const auto path = m_paths[node.id];
            node.fieldId = HashString(kFnvOffset, path);
            if (node.fieldId == 0u)
                node.fieldId = 1u;
        }
    }
    for (const auto& node : m_nodes)
    {
        if (node.fieldId != 0u)
            m_fieldIndex.emplace(node.fieldId, node.id);
    }
    RecomputeHash();
}

const SerializedTypeTreeNode* SerializedTypeTree::FindNode(const SerializedNodeId id) const
{
    return id == 0u || id > m_nodes.size() ? nullptr : &m_nodes[id - 1u];
}

const SerializedTypeTreeNode* SerializedTypeTree::GetParent(const SerializedNodeId id) const
{
    const auto* node = FindNode(id);
    return node == nullptr ? nullptr : FindNode(node->parent);
}

const SerializedTypeTreeNode* SerializedTypeTree::GetFirstChild(const SerializedNodeId id) const
{
    const auto* node = FindNode(id);
    return node == nullptr ? nullptr : FindNode(node->firstChild);
}

const SerializedTypeTreeNode* SerializedTypeTree::GetNextSibling(const SerializedNodeId id) const
{
    const auto* node = FindNode(id);
    return node == nullptr ? nullptr : FindNode(node->nextSibling);
}

const SerializedTypeTreeNode* SerializedTypeTree::FindPath(const std::string_view path) const
{
    const auto found = m_pathIndex.find(std::string(path));
    return found == m_pathIndex.end() ? nullptr : FindNode(found->second);
}

const SerializedTypeTreeNode* SerializedTypeTree::FindFieldId(const SerializedFieldId fieldId) const
{
    const auto found = m_fieldIndex.find(fieldId);
    return found == m_fieldIndex.end() ? nullptr : FindNode(found->second);
}

std::string SerializedTypeTree::GetPath(const SerializedNodeId id) const
{
    const auto found = m_paths.find(id);
    return found == m_paths.end() ? std::string {} : found->second;
}

void SerializedTypeTree::RebuildNodePath(const SerializedNodeId id, const std::string& parentPath)
{
    auto* node = const_cast<SerializedTypeTreeNode*>(FindNode(id));
    if (node == nullptr)
        return;
    const auto path = parentPath.empty() ? node->serializedName : parentPath + "." + node->serializedName;
    m_paths[id] = path;
    m_pathIndex[path] = id;
    for (const auto& alias : node->aliases)
    {
        if (alias.empty())
            continue;
        const auto aliasPath = parentPath.empty() ? alias : parentPath + "." + alias;
        m_pathIndex.emplace(aliasPath, id);
    }
    for (auto child = node->firstChild; child != 0u; )
    {
        const auto* childNode = FindNode(child);
        if (childNode == nullptr)
            break;
        RebuildNodePath(child, path);
        child = childNode->nextSibling;
    }
}

void SerializedTypeTree::RecomputeHash()
{
    uint64_t hash = kFnvOffset;
    for (const auto& node : m_nodes)
    {
        hash = HashBytes(hash, &node.id, sizeof(node.id));
        hash = HashBytes(hash, &node.parent, sizeof(node.parent));
        hash = HashBytes(hash, &node.fieldId, sizeof(node.fieldId));
        hash = HashString(hash, node.serializedName);
        hash = HashString(hash, node.typeName);
        hash = HashBytes(hash, &node.kind, sizeof(node.kind));
        hash = HashBytes(hash, &node.numericKind, sizeof(node.numericKind));
        hash = HashBytes(hash, &node.fixedByteSize, sizeof(node.fixedByteSize));
        const auto flags = static_cast<uint32_t>(node.flags);
        hash = HashBytes(hash, &flags, sizeof(flags));
        const auto aliasCount = static_cast<uint32_t>(node.aliases.size());
        hash = HashBytes(hash, &aliasCount, sizeof(aliasCount));
        for (const auto& alias : node.aliases)
            hash = HashString(hash, alias);
    }
    m_schemaHash = hash;
}

uint32_t SerializedReferenceTable::Intern(SerializedReferenceEntry entry)
{
    if (entry.isNull && !entry.identifier.IsValid() && entry.resourcePath.empty())
        return 0u;
    std::ostringstream key;
    key << entry.identifier.guid.ToString() << ":" << entry.identifier.localIdentifierInFile << ":" <<
        static_cast<int32_t>(entry.identifier.fileType) << ":" << entry.identifier.filePath << ":" << entry.resourcePath << ":" <<
        entry.expectedTypeName << ":" << (entry.isNull ? 1 : 0);
    const auto keyText = key.str();
    const auto found = m_index.find(keyText);
    if (found != m_index.end())
        return found->second;
    m_entries.push_back(std::move(entry));
    const auto slot = static_cast<uint32_t>(m_entries.size());
    m_index.emplace(keyText, slot);
    return slot;
}

const SerializedReferenceEntry* SerializedReferenceTable::Find(const uint32_t slot) const
{
    return slot == 0u || slot > m_entries.size() ? nullptr : &m_entries[slot - 1u];
}

void SerializedReferenceTable::ReplaceEntries(std::vector<SerializedReferenceEntry> entries)
{
    m_entries = std::move(entries);
    m_index.clear();
    for (uint32_t index = 0u; index < m_entries.size(); ++index)
    {
        const auto& entry = m_entries[index];
        std::ostringstream key;
        key << entry.identifier.guid.ToString() << ":" << entry.identifier.localIdentifierInFile << ":" <<
            static_cast<int32_t>(entry.identifier.fileType) << ":" << entry.identifier.filePath << ":" << entry.resourcePath << ":" <<
            entry.expectedTypeName << ":" << (entry.isNull ? 1 : 0);
        m_index[key.str()] = index + 1u;
    }
}

bool SerializedByteSlice::Equivalent(const SerializedByteSlice& other) const
{
    return valid == other.valid && typeSignature == other.typeSignature && bytes == other.bytes &&
        references == other.references && referenceRelocations == other.referenceRelocations;
}

SerializedByteStream::SerializedByteStream(std::shared_ptr<const SerializedTypeTree> schema)
    : m_schema(std::move(schema))
{
}

void SerializedByteStream::SetSchema(std::shared_ptr<const SerializedTypeTree> schema)
{
    m_schema = std::move(schema);
    MarkDataChanged(true, true);
}

void SerializedByteStream::SetBytes(std::vector<uint8_t> bytes)
{
    m_bytes = std::move(bytes);
    MarkDataChanged(true, true);
}

void SerializedByteStream::SetReferences(std::shared_ptr<SerializedReferenceTable> references)
{
    m_references = references != nullptr ? std::move(references) : std::make_shared<SerializedReferenceTable>();
}

void SerializedByteStream::MarkDataChanged(const bool layoutChanged, const bool structureChanged)
{
    ++m_dataRevision;
    if (layoutChanged)
        ++m_layoutRevision;
    if (structureChanged)
        ++m_structureRevision;
}

bool SerializedByteStream::Read(const size_t offset, void* output, const size_t size) const
{
    if (output == nullptr && size != 0u || offset > m_bytes.size() || size > m_bytes.size() - offset)
        return false;
    if (size != 0u)
        std::memcpy(output, m_bytes.data() + offset, size);
    return true;
}

bool SerializedByteStream::Write(const size_t offset, const void* input, const size_t size)
{
    if (input == nullptr && size != 0u || offset > m_bytes.size() || size > m_bytes.size() - offset)
        return false;
    if (size != 0u)
        std::memcpy(m_bytes.data() + offset, input, size);
    MarkDataChanged(false, false);
    return true;
}

bool SerializedByteStream::Replace(const size_t offset, const size_t length, const std::span<const uint8_t> replacement)
{
    if (offset > m_bytes.size() || length > m_bytes.size() - offset)
        return false;
    m_bytes.erase(m_bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        m_bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
    m_bytes.insert(m_bytes.begin() + static_cast<std::ptrdiff_t>(offset), replacement.begin(), replacement.end());
    MarkDataChanged(true, false);
    return true;
}

std::optional<SerializedByteSlice> SerializedByteStream::CaptureSlice(
    const size_t offset,
    const size_t length,
    const uint64_t typeSignature) const
{
    if (offset > m_bytes.size() || length > m_bytes.size() - offset)
        return std::nullopt;
    SerializedByteSlice result;
    result.bytes.assign(m_bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        m_bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
    result.typeSignature = typeSignature;
    if (m_references != nullptr)
        result.references = m_references->CopyEntries();
    if (m_schema != nullptr && length != 0u)
    {
        if (const auto* root = m_schema->FindNode(m_schema->GetRoot()); root != nullptr)
            CollectReferenceRelocations(*m_schema, *root, m_bytes, 0u, offset, offset + length, result.referenceRelocations);
    }
    result.valid = true;
    return result;
}

SerializedByteCursor::SerializedByteCursor(
    std::shared_ptr<SerializedByteStream> stream,
    const SerializedNodeId node,
    const size_t offset,
    const size_t length,
    std::string path)
    : m_stream(std::move(stream))
    , m_node(node)
    , m_offset(offset)
    , m_length(length)
    , m_path(std::move(path))
{
}

bool SerializedByteCursor::IsValid() const
{
    return m_stream != nullptr && m_stream->IsValid() && m_stream->GetSchema()->FindNode(m_node) != nullptr &&
        m_offset <= m_stream->GetBytes().size() && m_length <= m_stream->GetBytes().size() - m_offset;
}

bool SerializedByteCursor::Read(void* output, const size_t size, const size_t relativeOffset) const
{
    return IsValid() && relativeOffset <= m_length && size <= m_length - relativeOffset &&
        m_stream->Read(m_offset + relativeOffset, output, size);
}

bool SerializedByteCursor::Write(const void* input, const size_t size, const size_t relativeOffset)
{
    return IsValid() && relativeOffset <= m_length && size <= m_length - relativeOffset &&
        m_stream->Write(m_offset + relativeOffset, input, size);
}

std::optional<SerializedByteSlice> SerializedByteCursor::Capture(const uint64_t typeSignature) const
{
    return IsValid() ? m_stream->CaptureSlice(m_offset, m_length, typeSignature) : std::nullopt;
}

std::shared_ptr<SerializedTypeTree> NativeSerializedStreamCodec::BuildTypeTree(
    const meta::Type& type,
    const SerializedTypeTreeBuildOptions& options)
{
    auto tree = std::make_shared<SerializedTypeTree>();
    AddTypeNode(*tree, type, options.rootName.empty() ? "root" : options.rootName, 0u);
    tree->RebuildIndexes();
    return tree;
}

bool NativeSerializedStreamCodec::EncodeVariant(
    const meta::Variant& value,
    const SerializedTypeTree& tree,
    SerializedReferenceTable& references,
    std::vector<uint8_t>& output,
    std::string* error,
    const size_t alignmentOrigin)
{
    const auto* root = tree.FindNode(tree.GetRoot());
    if (root == nullptr || !value.IsValid())
    {
        if (error != nullptr) *error = "Cannot encode an invalid Variant or TypeTree.";
        return false;
    }
    BinaryBufferWriter writer(alignmentOrigin);
    if (!WriteVariantNode(value, tree, *root, references, writer, error))
        return false;
    output = std::move(writer.bytes);
    return true;
}

std::optional<meta::Variant> NativeSerializedStreamCodec::DecodeVariant(
    const std::vector<uint8_t>& bytes,
    const SerializedTypeTree& tree,
    const meta::Type& type,
    const SerializedReferenceTable& references,
    std::string* error,
    const size_t alignmentOrigin)
{
    const auto* root = tree.FindNode(tree.GetRoot());
    if (root == nullptr)
    {
        if (error != nullptr) *error = "Cannot decode without a TypeTree root.";
        return std::nullopt;
    }
    BinaryBufferReader reader(bytes, alignmentOrigin);
    auto result = ReadVariantNode(reader, tree, *root, type, references, error);
    if (!result.has_value() || !reader.IsComplete())
    {
        if (error != nullptr && error->empty()) *error = "Serialized byte stream has trailing or malformed data.";
        return std::nullopt;
    }
    return result;
}

bool NativeSerializedStreamCodec::RelocateReferenceSlots(
    std::vector<uint8_t>& bytes,
    const SerializedTypeTree& tree,
    const SerializedReferenceTable& sourceReferences,
    SerializedReferenceTable& destinationReferences,
    std::string* error)
{
    const auto* root = tree.FindNode(tree.GetRoot());
    if (root == nullptr)
    {
        if (error != nullptr) *error = "Cannot relocate references without a TypeTree root.";
        return false;
    }
    return RelocateReferenceSlotsNode(
        bytes,
        tree,
        *root,
        0u,
        sourceReferences,
        destinationReferences,
        error);
}

std::optional<size_t> NativeSerializedStreamCodec::GetValueByteSize(
    const SerializedByteStream& stream, SerializedNodeId id, size_t offset)
{
    const auto* node=stream.GetSchema()?stream.GetSchema()->FindNode(id):nullptr;
    return node?CalculateNodeSize(*stream.GetSchema(),*node,stream.GetBytes(),offset):std::nullopt;
}

bool NativeSerializedStreamCodec::RepackValueBytes(const SerializedTypeTree& tree,SerializedNodeId id,
    std::span<const uint8_t> bytes,size_t sourceOrigin,size_t destinationOrigin,std::vector<uint8_t>& output)
{
    // Only the low bits affect alignment; never allocate a prefix proportional
    // to an element's position in a large stream.
    const size_t sourceBase=sourceOrigin&3u,destinationBase=destinationOrigin&3u;
    std::vector<uint8_t> source(sourceBase,0);
    source.insert(source.end(),bytes.begin(),bytes.end());
    std::vector<uint8_t> result;
    const auto copy=[&](auto&& self,const SerializedTypeTreeNode& node,size_t offset)->std::optional<size_t>
    {
        const auto original=CalculateNodeSize(tree,node,source,offset);
        if(!original) return {};
        if(HasSerializedNodeFlag(node.flags,SerializedNodeFlags::Array))
        {
            uint32_t count=0;if(!ReadU32At(source,offset,count)) return {};
            result.insert(result.end(),source.begin()+offset,source.begin()+offset+sizeof(count));
            auto* element=FirstChild(tree,node);if(!element) return {};
            size_t cursor=offset+sizeof(count);
            for(uint32_t i=0;i<count;++i)
            {auto length=self(self,*element,cursor);if(!length) return {};cursor+=*length;}
        }
        else if(node.fixedByteSize<0 && node.kind!=SerializedNodeKind::String && FirstChild(tree,node))
        {
            size_t cursor=offset;
            for(auto* child=FirstChild(tree,node);child;child=tree.FindNode(child->nextSibling))
            {auto length=self(self,*child,cursor);if(!length) return {};cursor+=*length;}
        }
        else
        {
            auto unpadded=node;
            unpadded.flags=static_cast<SerializedNodeFlags>(static_cast<uint32_t>(node.flags)&
                ~static_cast<uint32_t>(SerializedNodeFlags::Align4After));
            const auto length=CalculateNodeSize(tree,unpadded,source,offset);
            if(!length) return {};
            result.insert(result.end(),source.begin()+offset,source.begin()+offset+*length);
        }
        if(HasSerializedNodeFlag(node.flags,SerializedNodeFlags::Align4After))
            result.resize(Align4Size(destinationBase+result.size())-destinationBase,0);
        return original;
    };
    const auto* node=tree.FindNode(id);
    if(!node) return false;
    const auto consumed=copy(copy,*node,sourceBase);
    if(!consumed || *consumed!=bytes.size()) return false;
    output=std::move(result);return true;
}

std::optional<SerializedByteCursor> NativeSerializedStreamCodec::ResolveCursor(
    const std::shared_ptr<SerializedByteStream>& stream,
    const std::string_view path)
{
    if (stream == nullptr || !stream->IsValid())
        return std::nullopt;
    const auto* exact = stream->GetSchema()->FindPath(path);
    if (exact != nullptr)
    {
        size_t offset = 0u;
        const auto* root = stream->GetSchema()->FindNode(stream->GetSchema()->GetRoot());
        if (root == nullptr)
            return std::nullopt;
        if (exact->id != root->id)
        {
            std::vector<const SerializedTypeTreeNode*> chain;
            for (auto current = exact; current != nullptr && current->id != root->id; current = stream->GetSchema()->FindNode(current->parent))
                chain.push_back(current);
            std::reverse(chain.begin(), chain.end());
            const auto* current = root;
            for (const auto* child : chain)
            {
                for (auto sibling = FirstChild(*stream->GetSchema(), *current); sibling != nullptr && sibling->id != child->id; sibling = stream->GetSchema()->FindNode(sibling->nextSibling))
                {
                    const auto siblingSize = CalculateNodeSize(*stream->GetSchema(), *sibling, stream->GetBytes(), offset);
                    if (!siblingSize.has_value())
                        return std::nullopt;
                    offset += *siblingSize;
                }
                current = child;
            }
        }
        const auto size = CalculateNodeSize(*stream->GetSchema(), *exact, stream->GetBytes(), offset);
        return size.has_value()
            ? std::optional<SerializedByteCursor>(SerializedByteCursor(stream, exact->id, offset, *size, std::string(path)))
            : std::nullopt;
    }

    const auto arrayPath = ParseArrayPath(path);
    if (!arrayPath.has_value())
    {
        const auto dot=path.rfind('.');
        if(dot==std::string_view::npos) return std::nullopt;
        const auto parent=ResolveCursor(stream,path.substr(0,dot));
        if(!parent) return std::nullopt;
        const auto* parentNode=stream->GetSchema()->FindNode(parent->GetNodeId());
        if(!parentNode || HasSerializedNodeFlag(parentNode->flags,SerializedNodeFlags::Array)) return std::nullopt;
        size_t offset=parent->GetByteOffset();
        const auto field=path.substr(dot+1);
        for(auto* child=FirstChild(*stream->GetSchema(),*parentNode);child;
            child=stream->GetSchema()->FindNode(child->nextSibling))
        {
            const auto size=CalculateNodeSize(*stream->GetSchema(),*child,stream->GetBytes(),offset);
            if(!size) return std::nullopt;
            if(child->serializedName==field) return SerializedByteCursor(stream,child->id,offset,*size,std::string(path));
            offset+=*size;
        }
        return std::nullopt;
    }
    auto parent = ResolveCursor(stream, arrayPath->parent);
    if (!parent.has_value()) return std::nullopt;
    const auto* arrayNode = stream->GetSchema()->FindNode(parent->GetNodeId());
    if (arrayNode == nullptr || !HasSerializedNodeFlag(arrayNode->flags, SerializedNodeFlags::Array))
        return std::nullopt;
    uint32_t count = 0u;
    if (!parent->Read(&count, sizeof(count)))
        return std::nullopt;
    if (arrayPath->index >= count)
        return std::nullopt;
    const auto* element = FirstChild(*stream->GetSchema(), *arrayNode);
    if (element == nullptr)
        return std::nullopt;
    size_t elementOffset = parent->GetByteOffset() + sizeof(uint32_t);
    for (size_t index = 0u; index < arrayPath->index; ++index)
    {
        const auto elementSize = CalculateNodeSize(*stream->GetSchema(), *element, stream->GetBytes(), elementOffset);
        if (!elementSize.has_value())
            return std::nullopt;
        elementOffset += *elementSize;
    }
    const auto elementSize = CalculateNodeSize(*stream->GetSchema(), *element, stream->GetBytes(), elementOffset);
    return elementSize.has_value()
        ? std::optional<SerializedByteCursor>(SerializedByteCursor(stream, element->id, elementOffset, *elementSize, std::string(path)))
        : std::nullopt;
}
} // namespace NLS::Engine::Serialize
