#pragma once

// Typed foundation for the Niagara-equivalent compiler.
//
// The existing runtime still consumes the compact Value/ValueType path. This
// header is the migration boundary: layouts, parameter maps, structs and data
// interface contracts are represented independently so graph translation,
// CPU bytecode and GPU shader code can share one typed description.

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace NLS::VFX::Typed
{
using TypeId = uint32_t;
inline constexpr TypeId kInvalidType = 0;
inline constexpr TypeId kVoid = 1;
inline constexpr TypeId kBool = 2;
inline constexpr TypeId kInt32 = 3;
inline constexpr TypeId kFloat = 4;
inline constexpr TypeId kFloat2 = 5;
inline constexpr TypeId kFloat3 = 6;
inline constexpr TypeId kFloat4 = 7;
inline constexpr TypeId kFirstUserType = 64;

enum class TypeKind : uint8_t
{
    Void,
    Bool,
    Int32,
    Float,
    Float2,
    Float3,
    Float4,
    Struct,
    DataInterface
};

struct TypeField
{
    std::string name;
    TypeId type = kInvalidType;
    uint32_t offset = 0;
};

struct TypeDesc
{
    TypeId id = kInvalidType;
    TypeKind kind = TypeKind::Void;
    std::string name;
    uint32_t size = 0;
    uint32_t alignment = 1;
    std::vector<TypeField> fields;

    [[nodiscard]] bool IsStruct() const { return kind == TypeKind::Struct; }
    [[nodiscard]] bool IsVector() const
    {
        return kind == TypeKind::Float2 || kind == TypeKind::Float3 || kind == TypeKind::Float4;
    }
};

struct Value
{
    TypeId type = kInvalidType;
    // Scalar and vector payloads. Struct payloads live in fields.
    std::array<float, 4> scalar{};
    std::vector<Value> fields;
};

struct FieldSpec
{
    std::string name;
    TypeId type = kInvalidType;
};

class TypeRegistry
{
public:
    TypeRegistry()
    {
        AddBuiltin(kVoid, TypeKind::Void, "Void", 0, 1);
        AddBuiltin(kBool, TypeKind::Bool, "Bool", 4, 4);
        AddBuiltin(kInt32, TypeKind::Int32, "Int32", 4, 4);
        AddBuiltin(kFloat, TypeKind::Float, "Float", 4, 4);
        AddBuiltin(kFloat2, TypeKind::Float2, "Float2", 8, 8);
        AddBuiltin(kFloat3, TypeKind::Float3, "Float3", 12, 4);
        AddBuiltin(kFloat4, TypeKind::Float4, "Float4", 16, 16);
    }

    [[nodiscard]] const TypeDesc* Get(TypeId id) const
    {
        const auto found = m_types.find(id);
        return found == m_types.end() ? nullptr : &found->second;
    }

    [[nodiscard]] std::optional<TypeId> Find(std::string_view name) const
    {
        const auto found = m_names.find(std::string(name));
        return found == m_names.end() ? std::nullopt : std::optional<TypeId>(found->second);
    }

    // Struct fields are laid out in declaration order using natural alignment.
    // Nested structs are supported because AddStruct only accepts existing
    // types; recursive declarations must be introduced in a later pass.
    TypeId AddStruct(std::string name, std::span<const FieldSpec> fields, std::string& error)
    {
        if (name.empty()) { error = "Struct name is empty"; return kInvalidType; }
        if (m_names.contains(name)) { error = "Duplicate type name: " + name; return kInvalidType; }
        if (fields.empty()) { error = "Struct requires at least one field"; return kInvalidType; }
        TypeDesc desc;
        desc.id = m_nextType++;
        desc.kind = TypeKind::Struct;
        desc.name = std::move(name);
        uint32_t offset = 0, alignment = 1;
        std::unordered_map<std::string, bool> fieldNames;
        for (const auto& field : fields)
        {
            const auto* type = Get(field.type);
            if (!type || field.type == kVoid) { error = "Struct field has an invalid type: " + field.name; return kInvalidType; }
            if (field.name.empty() || !fieldNames.emplace(field.name, true).second)
            { error = "Struct field name is empty or duplicated: " + field.name; return kInvalidType; }
            offset = AlignUp(offset, type->alignment);
            desc.fields.push_back({field.name, field.type, offset});
            offset += type->size;
            alignment = (std::max)(alignment, type->alignment);
        }
        desc.alignment = alignment;
        desc.size = AlignUp(offset, alignment);
        const auto id = desc.id;
        m_names.emplace(desc.name, id);
        m_types.emplace(id, std::move(desc));
        error.clear();
        return id;
    }

    [[nodiscard]] bool ValidateValue(const Value& value, std::string& error) const
    {
        const auto* type = Get(value.type);
        if (!type) { error = "Value has an unknown type"; return false; }
        if (type->kind == TypeKind::Struct)
        {
            if (value.fields.size() != type->fields.size())
            { error = "Struct value field count does not match its type"; return false; }
            for (size_t index = 0; index < value.fields.size(); ++index)
            {
                if (value.fields[index].type != type->fields[index].type || !ValidateValue(value.fields[index], error))
                { error = "Struct field type mismatch: " + type->fields[index].name; return false; }
            }
        }
        else if (!value.fields.empty())
        {
            error = "Scalar/vector value cannot contain struct fields";
            return false;
        }
        error.clear();
        return true;
    }

private:
    static uint32_t AlignUp(uint32_t value, uint32_t alignment)
    {
        return alignment <= 1 ? value : (value + alignment - 1) / alignment * alignment;
    }

    void AddBuiltin(TypeId id, TypeKind kind, std::string name, uint32_t size, uint32_t alignment)
    {
        TypeDesc desc{id, kind, std::move(name), size, alignment, {}};
        m_names.emplace(desc.name, id);
        m_types.emplace(id, std::move(desc));
    }

    std::unordered_map<TypeId, TypeDesc> m_types;
    std::unordered_map<std::string, TypeId> m_names;
    TypeId m_nextType = kFirstUserType;
};

struct ParameterMapEntry
{
    std::string name;
    TypeId type = kInvalidType;
    uint32_t offset = 0;
    Value defaultValue;
};

struct ParameterMapLayout
{
    uint32_t size = 0;
    uint32_t alignment = 1;
    std::vector<ParameterMapEntry> entries;

    [[nodiscard]] const ParameterMapEntry* Find(std::string_view name) const
    {
        const auto found = std::find_if(entries.begin(), entries.end(),
            [&](const ParameterMapEntry& entry) { return entry.name == name; });
        return found == entries.end() ? nullptr : &*found;
    }
};

inline ParameterMapLayout BuildParameterMap(const TypeRegistry& registry,
    std::span<const ParameterMapEntry> source, std::string& error)
{
    ParameterMapLayout result;
    std::unordered_map<std::string, bool> names;
    uint32_t offset = 0;
    for (const auto& entry : source)
    {
        const auto* type = registry.Get(entry.type);
        if (!type || entry.type == kVoid) { error = "Parameter has an invalid type: " + entry.name; return {}; }
        if (entry.name.empty() || !names.emplace(entry.name, true).second)
        { error = "Parameter name is empty or duplicated: " + entry.name; return {}; }
        auto value = entry.defaultValue;
        if (value.type == kInvalidType) value.type = entry.type;
        if (value.type != entry.type || !registry.ValidateValue(value, error))
        { error = "Parameter default does not match its type: " + entry.name; return {}; }
        offset = (offset + type->alignment - 1) / type->alignment * type->alignment;
        ParameterMapEntry placed = entry;
        placed.offset = offset;
        placed.defaultValue = std::move(value);
        result.entries.push_back(std::move(placed));
        offset += type->size;
        result.alignment = (std::max)(result.alignment, type->alignment);
    }
    result.size = result.alignment > 1 ? (offset + result.alignment - 1) / result.alignment * result.alignment : offset;
    error.clear();
    return result;
}

enum class DataInterfaceUsage : uint32_t
{
    None = 0,
    CPU = 1u << 0,
    GPU = 1u << 1,
    Both = 3u
};

inline constexpr DataInterfaceUsage operator|(DataInterfaceUsage a, DataInterfaceUsage b)
{
    return static_cast<DataInterfaceUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline constexpr DataInterfaceUsage operator&(DataInterfaceUsage a, DataInterfaceUsage b)
{
    return static_cast<DataInterfaceUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

struct DataInterfaceFunction
{
    std::string name;
    std::vector<TypeId> arguments;
    TypeId result = kVoid;
};

struct DataInterfaceDesc
{
    std::string name;
    DataInterfaceUsage usage = DataInterfaceUsage::None;
    std::vector<DataInterfaceFunction> functions;

    [[nodiscard]] bool Supports(DataInterfaceUsage flag) const
    {
        return flag != DataInterfaceUsage::None && (usage & flag) == flag;
    }
};

struct DataInterfaceRegistry
{
    std::vector<DataInterfaceDesc> interfaces;

    [[nodiscard]] const DataInterfaceDesc* Find(std::string_view name) const
    {
        const auto found = std::find_if(interfaces.begin(), interfaces.end(),
            [&](const DataInterfaceDesc& entry) { return entry.name == name; });
        return found == interfaces.end() ? nullptr : &*found;
    }
};
} // namespace NLS::VFX::Typed
