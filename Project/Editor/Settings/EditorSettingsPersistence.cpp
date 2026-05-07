#include "Settings/EditorSettingsPersistence.h"

#include <Reflection/Field.h>
#include <Reflection/Variant.h>

#include <fstream>

#include "Math/Quaternion.h"
#include "Math/Vector3.h"
#include "Math/Vector4.h"
#include "Panels/ReflectedPropertyDrawer.h"
#include "Reflection/JsonConfig.h"

namespace
{
using NLS::Json;
using namespace NLS;

template <typename TValue>
TValue GetFieldValue(meta::Variant& p_instance, const meta::Field& p_field)
{
    return p_field.GetValue(p_instance).GetValue<TValue>();
}

template <typename TValue>
void SetFieldValue(meta::Variant& p_instance, const meta::Field& p_field, const TValue& p_value)
{
    meta::Variant updatedValue(const_cast<TValue&>(p_value), meta::variant_policy::NoCopy {});
    p_field.SetValue(p_instance, updatedValue);
}

Json SerializeVector3(const Maths::Vector3& p_value)
{
    return Json::object { { "x", p_value.x }, { "y", p_value.y }, { "z", p_value.z } };
}

Json SerializeVector4(const Maths::Vector4& p_value)
{
    return Json::object { { "x", p_value.x }, { "y", p_value.y }, { "z", p_value.z }, { "w", p_value.w } };
}

Json SerializeQuaternion(const Maths::Quaternion& p_value)
{
    return Json::object { { "x", p_value.x }, { "y", p_value.y }, { "z", p_value.z }, { "w", p_value.w } };
}

bool TryReadVector3(const Json& p_json, Maths::Vector3& p_value)
{
    if (!p_json.is_object())
        return false;
    p_value = Maths::Vector3(
        p_json["x"].number_value(),
        p_json["y"].number_value(),
        p_json["z"].number_value());
    return true;
}

bool TryReadVector4(const Json& p_json, Maths::Vector4& p_value)
{
    if (!p_json.is_object())
        return false;
    p_value = Maths::Vector4(
        p_json["x"].number_value(),
        p_json["y"].number_value(),
        p_json["z"].number_value(),
        p_json["w"].number_value());
    return true;
}

bool TryReadQuaternion(const Json& p_json, Maths::Quaternion& p_value)
{
    if (!p_json.is_object())
        return false;
    p_value = Maths::Quaternion(
        p_json["x"].number_value(),
        p_json["y"].number_value(),
        p_json["z"].number_value(),
        p_json["w"].number_value());
    return true;
}

Json SerializeField(meta::Variant& p_instance, const meta::Field& p_field)
{
    using NLS::Editor::Panels::ReflectedPropertySupport;

    switch (NLS::Editor::Panels::GetReflectedPropertySupport(p_field))
    {
    case ReflectedPropertySupport::Bool:
        return GetFieldValue<bool>(p_instance, p_field);
    case ReflectedPropertySupport::Int:
    case ReflectedPropertySupport::Enum:
        return GetFieldValue<int>(p_instance, p_field);
    case ReflectedPropertySupport::Float:
        return GetFieldValue<float>(p_instance, p_field);
    case ReflectedPropertySupport::String:
        return GetFieldValue<std::string>(p_instance, p_field);
    case ReflectedPropertySupport::Vector3:
        return SerializeVector3(GetFieldValue<Maths::Vector3>(p_instance, p_field));
    case ReflectedPropertySupport::Vector4:
        return SerializeVector4(GetFieldValue<Maths::Vector4>(p_instance, p_field));
    case ReflectedPropertySupport::Quaternion:
        return SerializeQuaternion(GetFieldValue<Maths::Quaternion>(p_instance, p_field));
    default:
        return {};
    }
}

void DeserializeField(meta::Variant& p_instance, const meta::Field& p_field, const Json& p_json)
{
    using NLS::Editor::Panels::ReflectedPropertySupport;

    switch (NLS::Editor::Panels::GetReflectedPropertySupport(p_field))
    {
    case ReflectedPropertySupport::Bool:
        if (p_json.is_bool())
            SetFieldValue(p_instance, p_field, p_json.bool_value());
        break;
    case ReflectedPropertySupport::Int:
    case ReflectedPropertySupport::Enum:
        if (p_json.is_number())
            SetFieldValue(p_instance, p_field, static_cast<int>(p_json.number_value()));
        break;
    case ReflectedPropertySupport::Float:
        if (p_json.is_number())
            SetFieldValue(p_instance, p_field, static_cast<float>(p_json.number_value()));
        break;
    case ReflectedPropertySupport::String:
        if (p_json.is_string())
            SetFieldValue(p_instance, p_field, p_json.string_value());
        break;
    case ReflectedPropertySupport::Vector3:
    {
        Maths::Vector3 value;
        if (TryReadVector3(p_json, value))
            SetFieldValue(p_instance, p_field, value);
        break;
    }
    case ReflectedPropertySupport::Vector4:
    {
        Maths::Vector4 value;
        if (TryReadVector4(p_json, value))
            SetFieldValue(p_instance, p_field, value);
        break;
    }
    case ReflectedPropertySupport::Quaternion:
    {
        Maths::Quaternion value;
        if (TryReadQuaternion(p_json, value))
            SetFieldValue(p_instance, p_field, value);
        break;
    }
    default:
        break;
    }
}
}

namespace NLS::Editor::Settings
{
bool EditorSettingsPersistence::Load(const std::filesystem::path& p_path, const EditorSettingsRegistry& p_registry)
{
    if (!std::filesystem::exists(p_path))
        return true;

    std::ifstream file(p_path);
    if (!file.is_open())
        return false;

    const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::string error;
    const auto root = Json::parse(content, error);
    if (!error.empty() || !root.is_object())
        return false;

    const auto& objects = root["objects"];
    if (!objects.is_object())
        return true;

    for (const auto& settingObject : p_registry.GetObjects())
    {
        const auto& objectJson = objects[settingObject.id];
        if (!objectJson.is_object())
            continue;

        const auto& fieldsJson = objectJson["fields"];
        if (!fieldsJson.is_object())
            continue;

        auto instance = settingObject.makeVariant();
        for (const auto& field : settingObject.type.GetFields())
        {
            if (!field.IsValid())
                continue;
            const auto& fieldJson = fieldsJson[field.GetName()];
            if (!fieldJson.is_null())
                DeserializeField(instance, field, fieldJson);
        }
    }

    return true;
}

bool EditorSettingsPersistence::Save(const std::filesystem::path& p_path, const EditorSettingsRegistry& p_registry)
{
    std::error_code error;
    std::filesystem::create_directories(p_path.parent_path(), error);
    if (error)
        return false;

    Json::object objects;
    for (const auto& settingObject : p_registry.GetObjects())
    {
        Json::object fields;
        auto instance = settingObject.makeVariant();
        for (const auto& field : settingObject.type.GetFields())
        {
            if (!field.IsValid())
                continue;

            const auto value = SerializeField(instance, field);
            if (!value.is_null())
                fields[field.GetName()] = value;
        }

        objects[settingObject.id] = Json::object {
            { "displayName", settingObject.displayName },
            { "categoryPath", settingObject.categoryPath },
            { "fields", fields }
        };
    }

    const Json root = Json::object {
        { "version", 1 },
        { "objects", objects }
    };

    std::ofstream file(p_path, std::ios::trunc);
    if (!file.is_open())
        return false;

    file << root.dump();
    return true;
}
}
