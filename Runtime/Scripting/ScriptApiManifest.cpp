#include "ScriptApiManifest.h"

#include "Json/json.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <type_traits>

namespace NLS::Scripting
{
namespace
{
using Json = nlohmann::json;

Json ValueToJson(const ScriptValue& value)
{
    return std::visit([](const auto& item) -> Json
    {
        using Value = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Value, std::monostate>) return nullptr;
        else if constexpr (std::is_same_v<Value, ScriptEnumValue>) return {{"kind", "enum"}, {"type", item.typeId}, {"value", item.value}};
        else if constexpr (std::is_same_v<Value, ScriptStructValue>) return {{"kind", "struct"}, {"type", item.typeId}, {"bytes", item.bytes}};
        else if constexpr (std::is_same_v<Value, NativeObjectHandle>) return {{"kind", "runtimeObject"}, {"value", item.value}};
        else if constexpr (std::is_same_v<Value, ScriptObjectReference>) return {{"kind", "objectIdentifier"}, {"guid", item.guid}, {"localIdentifier", item.localIdentifierInFile}, {"fileType", item.fileType}, {"filePath", item.filePath}};
        else if constexpr (std::is_same_v<Value, NLS::Maths::Vector2>) return {{"kind", "vector2"}, {"x", item.x}, {"y", item.y}};
        else if constexpr (std::is_same_v<Value, NLS::Maths::Vector3>) return {{"kind", "vector3"}, {"x", item.x}, {"y", item.y}, {"z", item.z}};
        else if constexpr (std::is_same_v<Value, NLS::Maths::Vector4>) return {{"kind", "vector4"}, {"x", item.x}, {"y", item.y}, {"z", item.z}, {"w", item.w}};
        else if constexpr (std::is_same_v<Value, NLS::Maths::Quaternion>) return {{"kind", "quaternion"}, {"x", item.x}, {"y", item.y}, {"z", item.z}, {"w", item.w}};
        else if constexpr (std::is_same_v<Value, NLS::Maths::Color>) return {{"kind", "color"}, {"r", item.r}, {"g", item.g}, {"b", item.b}, {"a", item.a}};
        else return {{"kind", "scalar"}, {"value", item}};
    }, value);
}

bool ValueFromJson(const Json& json, ScriptValue& output, ScriptValueKind expectedKind = ScriptValueKind::Null)
{
    const auto kind = json.value("kind", std::string{});
    if (kind == "enum") { output = ScriptEnumValue{json.value("type", 0ull), json.value("value", 0ll)}; return true; }
    if (kind == "struct") { output = ScriptStructValue{json.value("type", 0ull), json.value("bytes", std::vector<uint8_t>{})}; return true; }
    if (kind == "runtimeObject") { output = NativeObjectHandle{json.value("value", 0ull)}; return true; }
    if (kind == "objectIdentifier") { output = ScriptObjectReference{json.value("guid", std::string{}), json.value("localIdentifier", int64_t{}), json.value("fileType", int32_t{}), json.value("filePath", std::string{})}; return true; }
    if (kind == "vector2") { output = NLS::Maths::Vector2(json.value("x", 0.0f), json.value("y", 0.0f)); return true; }
    if (kind == "vector3") { output = NLS::Maths::Vector3(json.value("x", 0.0f), json.value("y", 0.0f), json.value("z", 0.0f)); return true; }
    if (kind == "vector4") { output = NLS::Maths::Vector4(json.value("x", 0.0f), json.value("y", 0.0f), json.value("z", 0.0f), json.value("w", 0.0f)); return true; }
    if (kind == "quaternion") { output = NLS::Maths::Quaternion(json.value("x", 0.0f), json.value("y", 0.0f), json.value("z", 0.0f), json.value("w", 1.0f)); return true; }
    if (kind == "color") { output = NLS::Maths::Color(json.value("r", 1.0f), json.value("g", 1.0f), json.value("b", 1.0f), json.value("a", 1.0f)); return true; }
    if (kind != "scalar" || !json.contains("value")) return false;
    const auto& scalar = json["value"];
    // Scalar JSON numbers do not retain whether the source value was a float,
    // double, or one of the fixed-width integer kinds. Field descriptors carry
    // that contract, so use it when restoring defaults instead of falling
    // back to nlohmann::json's generic double representation.
    switch (expectedKind)
    {
    case ScriptValueKind::Bool:
        if (!scalar.is_boolean()) return false;
        output = scalar.get<bool>();
        return true;
    case ScriptValueKind::Int8: output = static_cast<int8_t>(scalar.get<int64_t>()); return true;
    case ScriptValueKind::UInt8: output = static_cast<uint8_t>(scalar.get<uint64_t>()); return true;
    case ScriptValueKind::Int16: output = static_cast<int16_t>(scalar.get<int64_t>()); return true;
    case ScriptValueKind::UInt16: output = static_cast<uint16_t>(scalar.get<uint64_t>()); return true;
    case ScriptValueKind::Int32: output = static_cast<int32_t>(scalar.get<int64_t>()); return true;
    case ScriptValueKind::UInt32: output = static_cast<uint32_t>(scalar.get<uint64_t>()); return true;
    case ScriptValueKind::Int64: output = scalar.get<int64_t>(); return true;
    case ScriptValueKind::UInt64: output = scalar.get<uint64_t>(); return true;
    case ScriptValueKind::Float: output = scalar.get<float>(); return true;
    case ScriptValueKind::Double: output = scalar.get<double>(); return true;
    case ScriptValueKind::String:
        if (!scalar.is_string()) return false;
        output = scalar.get<std::string>();
        return true;
    default:
        break;
    }
    if (scalar.is_boolean()) output = scalar.get<bool>();
    else if (scalar.is_number_integer()) output = scalar.get<int64_t>();
    else if (scalar.is_number_unsigned()) output = scalar.get<uint64_t>();
    else if (scalar.is_number_float()) output = scalar.get<double>();
    else if (scalar.is_string()) output = scalar.get<std::string>();
    else return false;
    return true;
}

Json TypeToJson(const ScriptType& type)
{
    return {
        {"id", type.id},
        {"kind", static_cast<uint32_t>(type.kind)},
        {"name", type.name},
        {"size", type.size},
        {"alignment", type.alignment},
        {"portable", type.portable}};
}

ScriptType TypeFromJson(const Json& json)
{
    ScriptType type;
    type.id = json.value("id", 0ull);
    type.kind = static_cast<ScriptValueKind>(json.value("kind", 0u));
    type.name = json.value("name", std::string{});
    type.size = json.value("size", 0u);
    type.alignment = json.value("alignment", 0u);
    type.portable = json.value("portable", true);
    return type;
}

Json ParameterToJson(const ScriptParameterDescriptor& parameter)
{
    return {{"name", parameter.name}, {"type", TypeToJson(parameter.type)}};
}

ScriptParameterDescriptor ParameterFromJson(const Json& json)
{
    return {TypeFromJson(json.value("type", Json::object())), json.value("name", std::string{})};
}
}

std::string ScriptApiManifest::ToJson(const ScriptApiDatabase& database)
{
    Json root;
    root["version"] = 2;
    root["schemaHash"] = database.GetSchemaHashHex();
    root["types"] = Json::array();
    std::vector<const ScriptTypeDescriptor*> types;
    types.reserve(database.GetTypes().size());
    for (const auto& type : database.GetTypes())
        types.push_back(&type);
    std::sort(types.begin(), types.end(), [](const auto* left, const auto* right)
    {
        if (left->name != right->name)
            return left->name < right->name;
        return left->id < right->id;
    });
    for (const auto* type : types)
    {
        Json typeJson = {
            {"id", type->id},
            {"name", type->name},
            {"methods", Json::array()},
            {"properties", Json::array()},
            {"fields", Json::array()}};
        std::vector<const ScriptMethodDescriptor*> methods;
        methods.reserve(type->methods.size());
        for (const auto& method : type->methods)
            methods.push_back(&method);
        std::sort(methods.begin(), methods.end(), [](const auto* left, const auto* right)
        {
            if (left->signature != right->signature)
                return left->signature < right->signature;
            return left->id < right->id;
        });
        for (const auto* method : methods)
        {
            Json methodJson = {
                {"id", method->id},
                {"name", method->name},
                {"returnType", TypeToJson(method->returnType)},
                {"callbacks", method->callbacks},
                {"scriptable", method->scriptable},
                {"static", method->isStatic},
                {"signature", method->signature},
                {"parameters", Json::array()}};
            for (const auto& parameter : method->parameters)
                methodJson["parameters"].push_back(ParameterToJson(parameter));
            typeJson["methods"].push_back(std::move(methodJson));
        }
        std::vector<const ScriptPropertyDescriptor*> properties;
        properties.reserve(type->properties.size());
        for (const auto& property : type->properties)
            properties.push_back(&property);
        std::sort(properties.begin(), properties.end(), [](const auto* left, const auto* right)
        {
            if (left->name != right->name)
                return left->name < right->name;
            return left->id < right->id;
        });
        for (const auto* property : properties)
        {
            typeJson["properties"].push_back({
                {"id", property->id},
                {"name", property->name},
                {"type", TypeToJson(property->type)},
                {"readable", property->readable},
                {"writable", property->writable},
                {"getterId", property->getterId},
                {"setterId", property->setterId},
                {"scriptable", property->scriptable}});
        }
        std::vector<const ScriptFieldDescriptor*> fields;
        fields.reserve(type->fields.size());
        for (const auto& field : type->fields)
            fields.push_back(&field);
        std::sort(fields.begin(), fields.end(), [](const auto* left, const auto* right)
        {
            if (left->name != right->name)
                return left->name < right->name;
            return left->id < right->id;
        });
        for (const auto* field : fields)
        {
            auto aliases = field->aliases;
            std::sort(aliases.begin(), aliases.end());
            typeJson["fields"].push_back({
                {"id", field->id},
                {"name", field->name},
                {"type", TypeToJson(field->type)},
                {"serialized", field->serialized},
                {"aliases", aliases}});
            if (field->defaultValue)
                typeJson["fields"].back()["default"] = ValueToJson(*field->defaultValue);
        }
        root["types"].push_back(std::move(typeJson));
    }
    return root.dump();
}

std::vector<uint8_t> ScriptApiManifest::ToBinary(const ScriptApiDatabase& database)
{
    const auto json = ToJson(database);
    const auto digest = database.GetSchemaHashBytes();
    constexpr char magic[] = "NLS_SCRIPT_API";
    std::vector<uint8_t> output;
    output.insert(output.end(), magic, magic + sizeof(magic));
    const auto append = [&output](auto value)
    {
        for (size_t index = 0; index < sizeof(value); ++index)
            output.push_back(static_cast<uint8_t>((static_cast<uint64_t>(value) >> (index * 8u)) & 0xffu));
    };
    append(BinaryVersion);
    output.insert(output.end(), digest.begin(), digest.end());
    append(static_cast<uint64_t>(json.size()));
    output.insert(output.end(), json.begin(), json.end());
    return output;
}

ScriptStatus ScriptApiManifest::FromJson(std::string_view jsonText, ScriptApiDatabase& output)
{
    try
    {
        const auto root = Json::parse(jsonText);
        if (root.value("version", 0) != 2 || !root.contains("types") || !root["types"].is_array())
            return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "Unsupported Script API manifest version.");

        ScriptApiDatabase parsed;
        for (const auto& typeJson : root["types"])
        {
            ScriptTypeDescriptor type;
            type.id = typeJson.value("id", 0ull);
            type.name = typeJson.value("name", std::string{});
            for (const auto& methodJson : typeJson.value("methods", Json::array()))
            {
                ScriptMethodDescriptor method;
                method.id = methodJson.value("id", 0ull);
                method.name = methodJson.value("name", std::string{});
                method.returnType = TypeFromJson(methodJson.value("returnType", Json::object()));
                method.callbacks = methodJson.value("callbacks", 0u);
                method.scriptable = methodJson.value("scriptable", false);
                method.isStatic = methodJson.value("static", false);
                method.signature = methodJson.value("signature", std::string{});
                for (const auto& parameterJson : methodJson.value("parameters", Json::array()))
                    method.parameters.push_back(ParameterFromJson(parameterJson));
                type.methods.push_back(std::move(method));
            }
            for (const auto& propertyJson : typeJson.value("properties", Json::array()))
            {
                ScriptPropertyDescriptor property;
                property.id = propertyJson.value("id", 0ull);
                property.name = propertyJson.value("name", std::string{});
                property.type = TypeFromJson(propertyJson.value("type", Json::object()));
                property.readable = propertyJson.value("readable", false);
                property.writable = propertyJson.value("writable", false);
                property.getterId = propertyJson.value("getterId", 0ull);
                property.setterId = propertyJson.value("setterId", 0ull);
                property.scriptable = propertyJson.value("scriptable", false);
                type.properties.push_back(std::move(property));
            }
            for (const auto& fieldJson : typeJson.value("fields", Json::array()))
            {
                ScriptFieldDescriptor field;
                field.id = fieldJson.value("id", 0ull);
                field.name = fieldJson.value("name", std::string{});
                field.type = TypeFromJson(fieldJson.value("type", Json::object()));
                field.serialized = fieldJson.value("serialized", true);
                field.aliases = fieldJson.value("aliases", std::vector<std::string>{});
                if (fieldJson.contains("default") && !fieldJson["default"].is_null())
                {
                    ScriptValue defaultValue;
                    if (!ValueFromJson(fieldJson["default"], defaultValue, field.type.kind))
                        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "Script API manifest contains an invalid field default value.");
                    field.defaultValue = std::move(defaultValue);
                }
                type.fields.push_back(std::move(field));
            }
            if (!parsed.RegisterType(std::move(type)))
                return ScriptStatus::Error(ScriptStatusCode::SchemaCollision, "Script API manifest contains a duplicate or colliding ID.");
        }
        const auto expectedHash = root.value("schemaHash", std::string{});
        if (expectedHash.empty())
            return ScriptStatus::Error(ScriptStatusCode::SchemaMismatch, "Script API manifest schemaHash is missing.");
        if (expectedHash != parsed.GetSchemaHashHex())
            return ScriptStatus::Error(ScriptStatusCode::SchemaMismatch, "Script API manifest schema hash is invalid.");
        output = std::move(parsed);
        return ScriptStatus::Ok();
    }
    catch (const std::exception& exception)
    {
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, exception.what());
    }
}

ScriptStatus ScriptApiManifest::FromBinary(std::span<const uint8_t> binary, ScriptApiDatabase& output)
{
    constexpr char magic[] = "NLS_SCRIPT_API";
    constexpr size_t magicSize = sizeof(magic);
    if (binary.size() < magicSize + sizeof(uint32_t) + 32u + sizeof(uint64_t)
        || std::memcmp(binary.data(), magic, magicSize) != 0)
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "Invalid Script API binary manifest header.");

    size_t offset = magicSize;
    const auto read = [&binary, &offset](size_t size)
    {
        uint64_t value = 0;
        for (size_t index = 0; index < size; ++index)
            value |= static_cast<uint64_t>(binary[offset + index]) << (index * 8u);
        offset += size;
        return value;
    };
    if (read(sizeof(uint32_t)) != BinaryVersion)
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "Unsupported Script API binary manifest version.");
    const auto expectedHash = std::vector<uint8_t>(binary.begin() + offset, binary.begin() + offset + 32);
    offset += 32;
    const auto jsonSize = read(sizeof(uint64_t));
    if (jsonSize > binary.size() - offset)
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "Truncated Script API binary manifest.");
    const auto status = FromJson(std::string_view(reinterpret_cast<const char*>(binary.data() + offset), static_cast<size_t>(jsonSize)), output);
    if (!status.Succeeded())
        return status;
    const auto actualHash = output.GetSchemaHashBytes();
    if (!std::equal(expectedHash.begin(), expectedHash.end(), actualHash.begin()))
        return ScriptStatus::Error(ScriptStatusCode::SchemaMismatch, "Script API binary manifest digest does not match its JSON payload.");
    return ScriptStatus::Ok();
}

ScriptStatus ScriptApiManifest::Write(const ScriptApiDatabase& database, const std::filesystem::path& directory)
{
    try
    {
        std::filesystem::create_directories(directory);
        std::ofstream jsonFile(directory / "ScriptApi.json", std::ios::binary | std::ios::trunc);
        std::ofstream binaryFile(directory / "ScriptApi.bin", std::ios::binary | std::ios::trunc);
        if (!jsonFile || !binaryFile)
            return ScriptStatus::Error(ScriptStatusCode::RuntimeError, "Unable to write Script API manifest files.");
        const auto json = ToJson(database);
        const auto binary = ToBinary(database);
        jsonFile.write(json.data(), static_cast<std::streamsize>(json.size()));
        binaryFile.write(reinterpret_cast<const char*>(binary.data()), static_cast<std::streamsize>(binary.size()));
        return ScriptStatus::Ok();
    }
    catch (const std::exception& exception)
    {
        return ScriptStatus::Error(ScriptStatusCode::RuntimeError, exception.what());
    }
}

ScriptStatus ScriptApiManifest::Read(const std::filesystem::path& directory, ScriptApiDatabase& output)
{
    try
    {
        std::ifstream file(directory / "ScriptApi.bin", std::ios::binary);
        if (!file)
            return ScriptStatus::Error(ScriptStatusCode::AssetNotLoaded, "Script API binary manifest was not found.");
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return FromBinary(bytes, output);
    }
    catch (const std::exception& exception)
    {
        return ScriptStatus::Error(ScriptStatusCode::RuntimeError, exception.what());
    }
}
}
