#include "ScriptFieldSerialization.h"

#include "Json/json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace NLS::Scripting
{
namespace
{
using Json = nlohmann::json;

Json ToJson(const ScriptValue& value)
{
    return std::visit(
        [](const auto& item) -> Json
        {
            using Value = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Value, std::monostate>)
                return nullptr;
            else if constexpr (std::is_same_v<Value, NativeObjectHandle>)
                return {{"kind", "runtimeObject"}, {"value", item.value}};
            else if constexpr (std::is_same_v<Value, ScriptObjectReference>)
                return {
                    {"kind", "objectIdentifier"},
                    {"guid", item.guid},
                    {"localIdentifier", item.localIdentifierInFile},
                    {"fileType", item.fileType},
                    {"filePath", item.filePath}};
            else if constexpr (std::is_same_v<Value, ScriptEnumValue>)
                return {{"kind", "enum"}, {"type", item.typeId}, {"value", item.value}};
            else if constexpr (std::is_same_v<Value, ScriptStructValue>)
                return {{"kind", "struct"}, {"type", item.typeId}, {"bytes", item.bytes}};
            else if constexpr (std::is_same_v<Value, NLS::Maths::Vector2>)
                return {{"kind", "vector2"}, {"x", item.x}, {"y", item.y}};
            else if constexpr (std::is_same_v<Value, NLS::Maths::Vector3>)
                return {{"kind", "vector3"}, {"x", item.x}, {"y", item.y}, {"z", item.z}};
            else if constexpr (std::is_same_v<Value, NLS::Maths::Vector4>)
                return {{"kind", "vector4"}, {"x", item.x}, {"y", item.y}, {"z", item.z}, {"w", item.w}};
            else if constexpr (std::is_same_v<Value, NLS::Maths::Quaternion>)
                return {{"kind", "quaternion"}, {"x", item.x}, {"y", item.y}, {"z", item.z}, {"w", item.w}};
            else if constexpr (std::is_same_v<Value, NLS::Maths::Color>)
                return {{"kind", "color"}, {"r", item.r}, {"g", item.g}, {"b", item.b}, {"a", item.a}};
            else
                return {{"kind", "scalar"}, {"value", item}};
        },
        value);
}

bool FromJson(const Json& json, ScriptValue& output)
{
    const auto kind = json.value("kind", std::string{});
    if (kind == "objectIdentifier")
    {
        output = ScriptObjectReference{
            json.value("guid", std::string{}),
            json.value("localIdentifier", int64_t{}),
            json.value("fileType", int32_t{}),
            json.value("filePath", std::string{})};
        return true;
    }
    if (kind == "runtimeObject" || kind == "object")
    {
        // The legacy "object" spelling is accepted for migration but is
        // intentionally converted to a persistent orphan by Deserialize;
        // runtime handles must never be written back to scene data.
        output = NativeObjectHandle{json.value("value", 0ull)};
        return true;
    }
    if (kind == "enum")
    {
        output = ScriptEnumValue{json.value("type", 0ull), json.value("value", 0ll)};
        return true;
    }
    if (kind == "struct")
    {
        output = ScriptStructValue{json.value("type", 0ull), json.value("bytes", std::vector<uint8_t>{})};
        return true;
    }
    if (kind == "vector2")
    {
        output = NLS::Maths::Vector2(json.value("x", 0.0f), json.value("y", 0.0f));
        return true;
    }
    if (kind == "vector3")
    {
        output = NLS::Maths::Vector3(json.value("x", 0.0f), json.value("y", 0.0f), json.value("z", 0.0f));
        return true;
    }
    if (kind == "vector4")
    {
        output = NLS::Maths::Vector4(json.value("x", 0.0f), json.value("y", 0.0f), json.value("z", 0.0f), json.value("w", 0.0f));
        return true;
    }
    if (kind == "quaternion")
    {
        output = NLS::Maths::Quaternion(json.value("x", 0.0f), json.value("y", 0.0f), json.value("z", 0.0f), json.value("w", 1.0f));
        return true;
    }
    if (kind == "color")
    {
        output = NLS::Maths::Color(json.value("r", 1.0f), json.value("g", 1.0f), json.value("b", 1.0f), json.value("a", 1.0f));
        return true;
    }
    if (kind != "scalar" || !json.contains("value"))
        return false;
    const auto& scalar = json["value"];
    if (scalar.is_boolean()) output = scalar.get<bool>();
    else if (scalar.is_number_integer()) output = scalar.get<int64_t>();
    else if (scalar.is_number_unsigned()) output = scalar.get<uint64_t>();
    else if (scalar.is_number_float()) output = scalar.get<double>();
    else if (scalar.is_string()) output = scalar.get<std::string>();
    else return false;
    return true;
}

bool IsKnownField(const ScriptTypeDescriptor* descriptor, ScriptFieldId id)
{
    if (!descriptor)
        return true;
    return std::any_of(descriptor->fields.begin(), descriptor->fields.end(), [id](const auto& field) { return field.id == id; });
}

const ScriptFieldDescriptor* FindField(const ScriptTypeDescriptor* descriptor, std::string_view name)
{
    if (!descriptor)
        return nullptr;
    for (const auto& field : descriptor->fields)
    {
        if (field.name == name || std::find(field.aliases.begin(), field.aliases.end(), name) != field.aliases.end())
            return &field;
    }
    return nullptr;
}

bool ConvertValue(const ScriptValue& input, const ScriptType& target, ScriptValue& output)
{
    if (target.kind == ScriptValueKind::Null)
    {
        output = input;
        return true;
    }
    if (target.kind == ScriptValueKind::Enum)
    {
        if (const auto* value = std::get_if<ScriptEnumValue>(&input))
        {
            if (value->typeId != 0 && target.id != 0 && value->typeId != target.id)
                return false;
            output = ScriptEnumValue{target.id, value->value};
            return true;
        }
    }
    if (target.kind == ScriptValueKind::String && std::holds_alternative<std::string>(input))
    {
        output = input;
        return true;
    }
    if (target.kind == ScriptValueKind::Bool && std::holds_alternative<bool>(input))
    {
        output = input;
        return true;
    }
    if (target.kind == ScriptValueKind::Vector2 && std::holds_alternative<NLS::Maths::Vector2>(input))
    {
        output = input;
        return true;
    }
    if (target.kind == ScriptValueKind::Vector3 && std::holds_alternative<NLS::Maths::Vector3>(input))
    {
        output = input;
        return true;
    }
    if (target.kind == ScriptValueKind::Vector4 && std::holds_alternative<NLS::Maths::Vector4>(input))
    {
        output = input;
        return true;
    }
    if (target.kind == ScriptValueKind::Quaternion && std::holds_alternative<NLS::Maths::Quaternion>(input))
    {
        output = input;
        return true;
    }
    if (target.kind == ScriptValueKind::Color && std::holds_alternative<NLS::Maths::Color>(input))
    {
        output = input;
        return true;
    }
    if (target.kind == ScriptValueKind::ObjectReference
        && (std::holds_alternative<NativeObjectHandle>(input)
            || std::holds_alternative<ScriptObjectReference>(input)))
    {
        output = input;
        return true;
    }
    if (target.kind == ScriptValueKind::Struct && std::holds_alternative<ScriptStructValue>(input))
    {
        output = input;
        return true;
    }

    const auto numeric = std::visit([](const auto& value) -> std::optional<long double>
    {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_arithmetic_v<Value> && !std::is_same_v<Value, bool>)
            return static_cast<long double>(value);
        return std::nullopt;
    }, input);
    if (!numeric)
        return false;

    if (target.kind == ScriptValueKind::Enum
        && std::isfinite(static_cast<double>(*numeric))
        && std::floor(*numeric) == *numeric
        && *numeric >= static_cast<long double>(std::numeric_limits<int64_t>::lowest())
        && *numeric <= static_cast<long double>(std::numeric_limits<int64_t>::max()))
    {
        output = ScriptEnumValue{target.id, static_cast<int64_t>(*numeric)};
        return true;
    }

    const auto integer = [&](auto tag) -> bool
    {
        using Target = decltype(tag);
        if (!std::isfinite(static_cast<double>(*numeric)) || std::floor(*numeric) != *numeric)
            return false;
        const auto value = *numeric;
        if (value < static_cast<long double>(std::numeric_limits<Target>::lowest())
            || value > static_cast<long double>(std::numeric_limits<Target>::max()))
            return false;
        output = static_cast<Target>(value);
        return true;
    };
    switch (target.kind)
    {
    case ScriptValueKind::Int8: return integer(int8_t{});
    case ScriptValueKind::UInt8: return integer(uint8_t{});
    case ScriptValueKind::Int16: return integer(int16_t{});
    case ScriptValueKind::UInt16: return integer(uint16_t{});
    case ScriptValueKind::Int32: return integer(int32_t{});
    case ScriptValueKind::UInt32: return integer(uint32_t{});
    case ScriptValueKind::Int64: return integer(int64_t{});
    case ScriptValueKind::UInt64: return integer(uint64_t{});
    case ScriptValueKind::Float:
        if (std::isfinite(static_cast<double>(*numeric))
            && *numeric >= -static_cast<long double>(std::numeric_limits<float>::max())
            && *numeric <= static_cast<long double>(std::numeric_limits<float>::max()))
        {
            output = static_cast<float>(*numeric);
            return true;
        }
        return false;
    case ScriptValueKind::Double:
        if (std::isfinite(static_cast<double>(*numeric)))
        {
            output = static_cast<double>(*numeric);
            return true;
        }
        return false;
    default:
        return false;
    }
}
}

ScriptStatus ScriptFieldSerialization::Serialize(const SerializedScriptFields& fields, const OrphanScriptFields& orphanFields, std::string& output)
{
    try
    {
        Json root = Json::object();
        for (const auto& [field, value] : fields)
        {
            if (std::holds_alternative<NativeObjectHandle>(value))
                return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "NativeObjectHandle values must be resolved to ObjectIdentifier before serialization.");
            root[std::to_string(field)] = ToJson(value);
        }
        for (const auto& [field, value] : orphanFields)
            if (!root.contains(std::to_string(field)))
            {
                const auto orphanJson = Json::parse(value);
                if (orphanJson.is_object())
                {
                    const auto kind = orphanJson.value("kind", std::string{});
                    if (kind == "runtimeObject" || kind == "object")
                        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "Orphan script fields cannot persist a NativeObjectHandle.");
                }
                root[std::to_string(field)] = orphanJson;
            }
        output = root.dump();
        return ScriptStatus::Ok();
    }
    catch (const std::exception& exception)
    {
        return ScriptStatus::Error(ScriptStatusCode::RuntimeError, exception.what());
    }
}

ScriptStatus ScriptFieldSerialization::Deserialize(std::string_view input, const ScriptTypeDescriptor* descriptor, SerializedScriptFields& fields, OrphanScriptFields& orphanFields)
{
    try
    {
        const auto root = Json::parse(input);
        if (!root.is_object())
            return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "Serialized script fields must be a JSON object.");
        fields.clear();
        orphanFields.clear();
        for (const auto& [key, value] : root.items())
        {
            uint64_t field = 0;
            const auto conversion = std::from_chars(key.data(), key.data() + key.size(), field);
            const bool numericKey = conversion.ec == std::errc{} && conversion.ptr == key.data() + key.size();
            const auto namedField = numericKey ? nullptr : FindField(descriptor, key);
            if (namedField)
                field = namedField->id;
            const ScriptFieldDescriptor* descriptorField = nullptr;
            if (descriptor && IsKnownField(descriptor, field))
            {
                const auto descriptorIterator = std::find_if(
                    descriptor->fields.begin(), descriptor->fields.end(),
                    [field](const auto& candidate) { return candidate.id == field; });
                if (descriptorIterator != descriptor->fields.end())
                    descriptorField = &*descriptorIterator;
            }
            if ((!numericKey && !namedField) || !IsKnownField(descriptor, field))
            {
                if (!numericKey)
                    field = MakeStableScriptId(key);
                orphanFields[field] = value.dump();
                continue;
            }
            ScriptValue converted;
            if (!FromJson(value, converted))
                return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "Serialized script field has an unsupported value.");
            if (std::holds_alternative<NativeObjectHandle>(converted))
            {
                // Runtime handles are process-local. Preserve the original
                // payload for an inspector warning, but never hydrate it as a
                // live field or write it back as a valid scene value.
                orphanFields[field] = value.dump();
                continue;
            }
            ScriptValue normalized;
            if (descriptorField && !ConvertValue(converted, descriptorField->type, normalized))
                return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "Serialized script field value does not match its declared type.");
            fields[field] = descriptorField ? std::move(normalized) : std::move(converted);
        }
        if (descriptor)
            for (const auto& field : descriptor->fields)
                if (field.defaultValue && !fields.contains(field.id))
                    fields[field.id] = *field.defaultValue;
        return ScriptStatus::Ok();
    }
    catch (const std::exception& exception)
    {
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, exception.what());
    }
}
}
