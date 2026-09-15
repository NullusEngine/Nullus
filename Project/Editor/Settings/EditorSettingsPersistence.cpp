#include "Settings/EditorSettingsPersistence.h"

#include <Reflection/Field.h>
#include <Reflection/Variant.h>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <type_traits>

#include "Inspector/PropertyTypeTraits.h"
#include "Math/Color.h"
#include "Math/Quaternion.h"
#include "Math/Rect.h"
#include "Math/Vector2.h"
#include "Math/Vector3.h"
#include "Math/Vector4.h"
#include "Engine/LayerMask.h"
#include "Reflection/JsonConfig.h"
#include "Rendering/Geometry/BoundingSphere.h"
#include "Rendering/Geometry/Bounds.h"
#include "Settings/EditorSettings.h"

namespace
{
using NLS::Json;
using namespace NLS;

constexpr int kEditorSettingsVersion = 2;

template <typename TValue>
TValue GetFieldValue(meta::Variant& p_instance, const meta::Field& p_field)
{
    return p_field.GetValue(p_instance).GetValue<TValue>();
}

template <typename TValue>
void SetFieldValue(meta::Variant& p_instance, const meta::Field& p_field, const TValue& p_value)
{
    TValue copy = p_value;
    meta::Variant updatedValue(copy);
    p_field.SetValue(p_instance, updatedValue);
}

Json SerializeVector3(const Maths::Vector3& p_value)
{
    return Json::object { { "x", p_value.x }, { "y", p_value.y }, { "z", p_value.z } };
}

Json SerializeVector2(const Maths::Vector2& p_value)
{
    return Json::object { { "x", p_value.x }, { "y", p_value.y } };
}

Json SerializeVector4(const Maths::Vector4& p_value)
{
    return Json::object { { "x", p_value.x }, { "y", p_value.y }, { "z", p_value.z }, { "w", p_value.w } };
}

Json SerializeQuaternion(const Maths::Quaternion& p_value)
{
    return Json::object { { "x", p_value.x }, { "y", p_value.y }, { "z", p_value.z }, { "w", p_value.w } };
}

Json SerializeRect(const Maths::Rect& p_value)
{
    return Json::object { { "x", p_value.x }, { "y", p_value.y }, { "width", p_value.width }, { "height", p_value.height } };
}

Json SerializeBounds(const Render::Geometry::Bounds& p_value)
{
    return Json::object { { "center", SerializeVector3(p_value.center) }, { "size", SerializeVector3(p_value.size) } };
}

Json SerializeColor(const Maths::Color& p_value)
{
    return Json::object { { "r", p_value.r }, { "g", p_value.g }, { "b", p_value.b }, { "a", p_value.a } };
}

Json SerializeBoundingSphere(const Render::Geometry::BoundingSphere& p_value)
{
    return Json::object { { "position", SerializeVector3(p_value.position) }, { "radius", p_value.radius } };
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

bool TryReadVector2(const Json& p_json, Maths::Vector2& p_value)
{
    if (!p_json.is_object())
        return false;
    p_value = Maths::Vector2(
        p_json["x"].number_value(),
        p_json["y"].number_value());
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

bool TryReadRect(const Json& p_json, Maths::Rect& p_value)
{
    if (!p_json.is_object())
        return false;
    p_value = Maths::Rect(
        p_json["x"].number_value(),
        p_json["y"].number_value(),
        p_json["width"].number_value(),
        p_json["height"].number_value());
    return true;
}

bool TryReadBounds(const Json& p_json, Render::Geometry::Bounds& p_value)
{
    if (!p_json.is_object())
        return false;
    return TryReadVector3(p_json["center"], p_value.center) &&
           TryReadVector3(p_json["size"], p_value.size);
}

bool TryReadColor(const Json& p_json, Maths::Color& p_value)
{
    if (!p_json.is_object())
        return false;
    p_value = Maths::Color(
        p_json["r"].number_value(),
        p_json["g"].number_value(),
        p_json["b"].number_value(),
        p_json["a"].number_value());
    return true;
}

bool TryReadBoundingSphere(const Json& p_json, Render::Geometry::BoundingSphere& p_value)
{
    if (!p_json.is_object() || !p_json["radius"].is_number())
        return false;
    return TryReadVector3(p_json["position"], p_value.position) &&
        ((p_value.radius = p_json["radius"].number_value()), true);
}

template <typename TValue>
bool TryReadInteger(const Json& p_json, TValue& p_value)
{
    static_assert(std::is_integral_v<TValue>);
    if (p_json.is_number())
    {
        const double number = p_json.number_value();
        if (!std::isfinite(number) || std::trunc(number) != number ||
            number < static_cast<double>(std::numeric_limits<TValue>::lowest()) ||
            number > static_cast<double>(std::numeric_limits<TValue>::max()))
        {
            return false;
        }
        p_value = static_cast<TValue>(number);
        return true;
    }

    if (!p_json.is_string())
        return false;

    if constexpr (std::is_signed_v<TValue>)
    {
        int64_t parsed = 0;
        const auto [end, error] = std::from_chars(
            p_json.string_value().data(),
            p_json.string_value().data() + p_json.string_value().size(),
            parsed);
        if (error != std::errc {} || end != p_json.string_value().data() + p_json.string_value().size() ||
            parsed < std::numeric_limits<TValue>::lowest() || parsed > std::numeric_limits<TValue>::max())
        {
            return false;
        }
        p_value = static_cast<TValue>(parsed);
    }
    else
    {
        uint64_t parsed = 0;
        const auto [end, error] = std::from_chars(
            p_json.string_value().data(),
            p_json.string_value().data() + p_json.string_value().size(),
            parsed);
        if (error != std::errc {} || end != p_json.string_value().data() + p_json.string_value().size() ||
            parsed > std::numeric_limits<TValue>::max())
        {
            return false;
        }
        p_value = static_cast<TValue>(parsed);
    }
    return true;
}

Json SerializeField(meta::Variant& p_instance, const meta::Field& p_field)
{
    using NLS::Editor::Inspector::GetInspectorPropertyKind;
    using NLS::Editor::Inspector::InspectorPropertyKind;

    switch (GetInspectorPropertyKind(p_field.GetType()))
    {
    case InspectorPropertyKind::Boolean:
        return GetFieldValue<bool>(p_instance, p_field);
    case InspectorPropertyKind::SignedInteger8:
        return static_cast<int>(GetFieldValue<int8_t>(p_instance, p_field));
    case InspectorPropertyKind::UnsignedInteger8:
        return static_cast<int>(GetFieldValue<uint8_t>(p_instance, p_field));
    case InspectorPropertyKind::SignedInteger16:
        return static_cast<int>(GetFieldValue<int16_t>(p_instance, p_field));
    case InspectorPropertyKind::UnsignedInteger16:
        return static_cast<int>(GetFieldValue<uint16_t>(p_instance, p_field));
    case InspectorPropertyKind::SignedInteger32:
        return static_cast<double>(GetFieldValue<int32_t>(p_instance, p_field));
    case InspectorPropertyKind::UnsignedInteger32:
        return static_cast<double>(GetFieldValue<uint32_t>(p_instance, p_field));
    case InspectorPropertyKind::SignedInteger64:
        return std::to_string(GetFieldValue<int64_t>(p_instance, p_field));
    case InspectorPropertyKind::UnsignedInteger64:
        return std::to_string(GetFieldValue<uint64_t>(p_instance, p_field));
    case InspectorPropertyKind::Enum:
    case InspectorPropertyKind::Flags:
        return p_field.GetValue(p_instance).ToInt();
    case InspectorPropertyKind::Float:
        return GetFieldValue<float>(p_instance, p_field);
    case InspectorPropertyKind::Double:
        return GetFieldValue<double>(p_instance, p_field);
    case InspectorPropertyKind::String:
        return GetFieldValue<std::string>(p_instance, p_field);
    case InspectorPropertyKind::Vector2:
        return SerializeVector2(GetFieldValue<Maths::Vector2>(p_instance, p_field));
    case InspectorPropertyKind::Rect:
        return SerializeRect(GetFieldValue<Maths::Rect>(p_instance, p_field));
    case InspectorPropertyKind::Bounds:
        return SerializeBounds(GetFieldValue<Render::Geometry::Bounds>(p_instance, p_field));
    case InspectorPropertyKind::BoundingSphere:
        return SerializeBoundingSphere(GetFieldValue<Render::Geometry::BoundingSphere>(p_instance, p_field));
    case InspectorPropertyKind::Vector3:
        return SerializeVector3(GetFieldValue<Maths::Vector3>(p_instance, p_field));
    case InspectorPropertyKind::Vector4:
        return SerializeVector4(GetFieldValue<Maths::Vector4>(p_instance, p_field));
    case InspectorPropertyKind::Quaternion:
        return SerializeQuaternion(GetFieldValue<Maths::Quaternion>(p_instance, p_field));
    case InspectorPropertyKind::Color:
        return SerializeColor(GetFieldValue<Maths::Color>(p_instance, p_field));
    case InspectorPropertyKind::LayerMask:
        return static_cast<double>(GetFieldValue<Engine::LayerMask>(p_instance, p_field).GetMask());
    default:
        return {};
    }
}

void DeserializeField(meta::Variant& p_instance, const meta::Field& p_field, const Json& p_json)
{
    using NLS::Editor::Inspector::GetInspectorPropertyKind;
    using NLS::Editor::Inspector::InspectorPropertyKind;

    switch (GetInspectorPropertyKind(p_field.GetType()))
    {
    case InspectorPropertyKind::Boolean:
        if (p_json.is_bool())
            SetFieldValue(p_instance, p_field, p_json.bool_value());
        break;
    case InspectorPropertyKind::SignedInteger8: { int8_t value; if (TryReadInteger(p_json, value)) SetFieldValue(p_instance, p_field, value); break; }
    case InspectorPropertyKind::UnsignedInteger8: { uint8_t value; if (TryReadInteger(p_json, value)) SetFieldValue(p_instance, p_field, value); break; }
    case InspectorPropertyKind::SignedInteger16: { int16_t value; if (TryReadInteger(p_json, value)) SetFieldValue(p_instance, p_field, value); break; }
    case InspectorPropertyKind::UnsignedInteger16: { uint16_t value; if (TryReadInteger(p_json, value)) SetFieldValue(p_instance, p_field, value); break; }
    case InspectorPropertyKind::SignedInteger32: { int32_t value; if (TryReadInteger(p_json, value)) SetFieldValue(p_instance, p_field, value); break; }
    case InspectorPropertyKind::UnsignedInteger32: { uint32_t value; if (TryReadInteger(p_json, value)) SetFieldValue(p_instance, p_field, value); break; }
    case InspectorPropertyKind::SignedInteger64: { int64_t value; if (TryReadInteger(p_json, value)) SetFieldValue(p_instance, p_field, value); break; }
    case InspectorPropertyKind::UnsignedInteger64: { uint64_t value; if (TryReadInteger(p_json, value)) SetFieldValue(p_instance, p_field, value); break; }
    case InspectorPropertyKind::Enum:
    case InspectorPropertyKind::Flags:
    {
        int value = 0;
        if (TryReadInteger(p_json, value))
            SetFieldValue(p_instance, p_field, value);
        break;
    }
    case InspectorPropertyKind::Float:
        if (p_json.is_number())
            SetFieldValue(p_instance, p_field, static_cast<float>(p_json.number_value()));
        break;
    case InspectorPropertyKind::Double:
        if (p_json.is_number())
            SetFieldValue(p_instance, p_field, p_json.number_value());
        break;
    case InspectorPropertyKind::String:
        if (p_json.is_string())
            SetFieldValue(p_instance, p_field, p_json.string_value());
        break;
    case InspectorPropertyKind::Vector2:
    {
        Maths::Vector2 value;
        if (TryReadVector2(p_json, value))
            SetFieldValue(p_instance, p_field, value);
        break;
    }
    case InspectorPropertyKind::Rect:
    {
        Maths::Rect value;
        if (TryReadRect(p_json, value))
            SetFieldValue(p_instance, p_field, value);
        break;
    }
    case InspectorPropertyKind::Bounds:
    {
        Render::Geometry::Bounds value;
        if (TryReadBounds(p_json, value))
            SetFieldValue(p_instance, p_field, value);
        break;
    }
    case InspectorPropertyKind::BoundingSphere:
    {
        Render::Geometry::BoundingSphere value;
        if (TryReadBoundingSphere(p_json, value))
            SetFieldValue(p_instance, p_field, value);
        break;
    }
    case InspectorPropertyKind::Vector3:
    {
        Maths::Vector3 value;
        if (TryReadVector3(p_json, value))
            SetFieldValue(p_instance, p_field, value);
        break;
    }
    case InspectorPropertyKind::Vector4:
    {
        Maths::Vector4 value;
        if (TryReadVector4(p_json, value))
            SetFieldValue(p_instance, p_field, value);
        break;
    }
    case InspectorPropertyKind::Quaternion:
    {
        Maths::Quaternion value;
        if (TryReadQuaternion(p_json, value))
            SetFieldValue(p_instance, p_field, value);
        break;
    }
    case InspectorPropertyKind::Color:
    {
        Maths::Color value;
        if (TryReadColor(p_json, value))
            SetFieldValue(p_instance, p_field, value);
        break;
    }
    case InspectorPropertyKind::LayerMask:
    {
        uint32_t mask = 0u;
        if (TryReadInteger(p_json, mask))
            SetFieldValue(p_instance, p_field, Engine::LayerMask(mask));
        break;
    }
    default:
        break;
    }
}

void ApplyPostLoadMigrations(const Json& p_root)
{
    const auto version = p_root["version"].is_number()
        ? static_cast<int>(p_root["version"].number_value())
        : 0;
    if (version >= kEditorSettingsVersion)
        return;

    auto& debugSettings = NLS::Editor::Settings::EditorSettings::GetDebugDrawSettingsObject();
    debugSettings.debugDrawCamera = true;
    debugSettings.debugDrawLighting = true;
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

    ApplyPostLoadMigrations(root);
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
        { "version", kEditorSettingsVersion },
        { "objects", objects }
    };

    std::ofstream file(p_path, std::ios::trunc);
    if (!file.is_open())
        return false;

    file << root.dump();
    return true;
}
}
