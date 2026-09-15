#include "Serialization/AccessorSerializedBackend.h"

#include <algorithm>
#include <functional>
#include <unordered_map>

#include <Components/UICanvasComponent.h>
#include <Components/Component.h>
#include <Components/TransformComponent.h>
#include <GameObject.h>
#include <Serialize/NativeSerializedStream.h>
#include <Scripting/ScriptComponent.h>
#include <Scripting/ScriptTypes.h>

#include "Serialization/NativeReflectionSerializedBackend.h"

namespace NLS::Editor::Serialization
{
namespace
{
using Engine::Serialize::NativeSerializedStreamCodec;
using Engine::Serialize::SerializedByteStream;
using Engine::Serialize::SerializedReferenceTable;
using Engine::Serialize::SerializedTypeTree;
using Engine::Serialize::SerializedTypeTreeNode;

struct Descriptor
{
    std::string name;
    // The TypeTree path can be nested even when the accessor itself addresses
    // one scalar field. `name` is the accessor field name; `path` is the
    // public SerializedProperty path.
    std::string path;
    uint64_t fieldId = 0u;
    std::vector<std::string> aliases;
    meta::Type type;
    bool readOnly = false;
    std::function<meta::Variant(Object&)> read;
    std::function<bool(Object&, const meta::Variant&)> write;
};

Object* Resolve(const SerializedTargetHandle& target)
{
    return NativeReflectionSerializedBackend::Resolve(target);
}

bool SameType(const meta::Type& left, const meta::Type& right)
{
    return left.IsValid() && right.IsValid() && (left == right || left.GetID() == right.GetID());
}

template <typename T>
bool IsVariant(const meta::Variant& value)
{
    return value.IsValid() && SameType(value.GetType(), NLS_TYPEOF(T));
}

uint64_t StableFieldId(const std::string_view ownerType, const std::string_view name)
{
    return static_cast<uint64_t>(std::hash<std::string> {}(
        std::string(ownerType) + "|" + std::string(name)));
}

std::string RootName(const std::string_view path)
{
    const auto dot = path.find('.');
    const auto array = path.find(".Array");
    const auto end = (std::min)(dot == std::string_view::npos ? path.size() : dot,
        array == std::string_view::npos ? path.size() : array);
    return std::string(path.substr(0u, end));
}

Descriptor Canvas(std::string name, const meta::Type& type,
    std::function<meta::Variant(Engine::Components::UICanvasComponent&)> read,
    std::function<bool(Engine::Components::UICanvasComponent&, const meta::Variant&)> write)
{
    Descriptor descriptor;
    descriptor.fieldId = StableFieldId(NLS_TYPEOF(Engine::Components::UICanvasComponent).GetName(), name);
    descriptor.name = std::move(name);
    descriptor.path = descriptor.name;
    descriptor.type = type;
    descriptor.read = [read = std::move(read)](Object& object)
    {
        auto* canvas = dynamic_cast<Engine::Components::UICanvasComponent*>(&object);
        return canvas == nullptr ? meta::Variant {} : read(*canvas);
    };
    descriptor.write = [write = std::move(write)](Object& object, const meta::Variant& value)
    {
        auto* canvas = dynamic_cast<Engine::Components::UICanvasComponent*>(&object);
        return canvas != nullptr && write(*canvas, value);
    };
    return descriptor;
}

Descriptor Transform(std::string name, const meta::Type& type,
    std::function<meta::Variant(Engine::Components::TransformComponent&)> read,
    std::function<bool(Engine::Components::TransformComponent&, const meta::Variant&)> write)
{
    Descriptor descriptor;
    descriptor.fieldId = StableFieldId(NLS_TYPEOF(Engine::Components::TransformComponent).GetName(), name);
    descriptor.name = std::move(name);
    descriptor.path = descriptor.name;
    descriptor.type = type;
    descriptor.read = [read = std::move(read)](Object& object)
    {
        auto* transform = dynamic_cast<Engine::Components::TransformComponent*>(&object);
        return transform == nullptr ? meta::Variant {} : read(*transform);
    };
    descriptor.write = [write = std::move(write)](Object& object, const meta::Variant& value)
    {
        auto* transform = dynamic_cast<Engine::Components::TransformComponent*>(&object);
        return transform != nullptr && write(*transform, value);
    };
    return descriptor;
}

Descriptor GameObjectField(std::string name, const meta::Type& type,
    std::function<meta::Variant(Engine::GameObject&)> read,
    std::function<bool(Engine::GameObject&, const meta::Variant&)> write)
{
    Descriptor descriptor;
    descriptor.fieldId = StableFieldId(NLS_TYPEOF(Engine::GameObject).GetName(), name);
    descriptor.name = std::move(name);
    descriptor.path = "GameObject." + descriptor.name;
    descriptor.type = type;
    descriptor.read = [read = std::move(read)](Object& object)
    {
        auto* gameObject = dynamic_cast<Engine::GameObject*>(&object);
        return gameObject == nullptr ? meta::Variant {} : read(*gameObject);
    };
    descriptor.write = [write = std::move(write)](Object& object, const meta::Variant& value)
    {
        auto* gameObject = dynamic_cast<Engine::GameObject*>(&object);
        return gameObject != nullptr && write(*gameObject, value);
    };
    return descriptor;
}

std::vector<Descriptor> GameObjectDescriptors()
{
    using GameObjectType = Engine::GameObject;
    return {
        GameObjectField("active", NLS_TYPEOF(bool),
            [](GameObjectType& value) { return meta::Variant(value.IsSelfActive()); },
            [](GameObjectType& value, const meta::Variant& next) { if (!IsVariant<bool>(next)) return false; value.SetActive(next.GetValue<bool>()); return true; }),
        GameObjectField("name", NLS_TYPEOF(std::string),
            [](GameObjectType& value) { return meta::Variant(value.GetName()); },
            [](GameObjectType& value, const meta::Variant& next) { if (!IsVariant<std::string>(next)) return false; value.SetName(next.GetValue<std::string>()); return true; }),
        GameObjectField("tag", NLS_TYPEOF(std::string),
            [](GameObjectType& value) { return meta::Variant(value.GetTag()); },
            [](GameObjectType& value, const meta::Variant& next) { if (!IsVariant<std::string>(next)) return false; value.SetTag(next.GetValue<std::string>()); return true; }),
        GameObjectField("layer", NLS_TYPEOF(int32_t),
            [](GameObjectType& value) { return meta::Variant(static_cast<int32_t>(value.GetLayer())); },
            [](GameObjectType& value, const meta::Variant& next) { if (!IsVariant<int32_t>(next)) return false; value.SetLayer(next.GetValue<int32_t>()); return true; })};
}

std::vector<Descriptor> ComponentHeaderDescriptors()
{
    Descriptor descriptor;
    descriptor.fieldId = StableFieldId("Component", "enabled");
    descriptor.name = "enabled";
    descriptor.path = descriptor.name;
    descriptor.type = NLS_TYPEOF(bool);
    descriptor.read = [](Object& object)
    {
        auto* component = dynamic_cast<Engine::Components::Component*>(&object);
        return component == nullptr ? meta::Variant {} : meta::Variant(component->IsSelfEnabled());
    };
    descriptor.write = [](Object& object, const meta::Variant& value)
    {
        auto* component = dynamic_cast<Engine::Components::Component*>(&object);
        if (component == nullptr || dynamic_cast<Engine::Components::TransformComponent*>(component) != nullptr ||
            !IsVariant<bool>(value))
        {
            return false;
        }
        component->SetEnabled(value.GetValue<bool>());
        return true;
    };
    return {std::move(descriptor)};
}

std::vector<Descriptor> TransformDescriptors()
{
    using TransformType = Engine::Components::TransformComponent;
    return {
        Transform("localPosition", NLS_TYPEOF(Maths::Vector3),
            [](TransformType& value) { return meta::Variant(value.GetLocalPosition()); },
            [](TransformType& value, const meta::Variant& next)
            {
                if (!IsVariant<Maths::Vector3>(next)) return false;
                value.SetLocalPosition(next.GetValue<Maths::Vector3>());
                return true;
            }),
        Transform("localRotation", NLS_TYPEOF(Maths::Quaternion),
            [](TransformType& value) { return meta::Variant(value.GetLocalRotation()); },
            [](TransformType& value, const meta::Variant& next)
            {
                if (!IsVariant<Maths::Quaternion>(next)) return false;
                value.SetLocalRotation(next.GetValue<Maths::Quaternion>());
                return true;
            }),
        Transform("localScale", NLS_TYPEOF(Maths::Vector3),
            [](TransformType& value) { return meta::Variant(value.GetLocalScale()); },
            [](TransformType& value, const meta::Variant& next)
            {
                if (!IsVariant<Maths::Vector3>(next)) return false;
                value.SetLocalScale(next.GetValue<Maths::Vector3>());
                return true;
            })};
}

std::vector<Descriptor> CanvasDescriptors()
{
    using CanvasType = Engine::Components::UICanvasComponent;
    return {
        Canvas("document", NLS_TYPEOF(Engine::UI::UIDocumentReference),
            [](CanvasType& value) { return meta::Variant(value.GetDocumentReference()); },
            [](CanvasType& value, const meta::Variant& next) { if (!IsVariant<Engine::UI::UIDocumentReference>(next)) return false; value.SetDocumentReference(next.GetValue<Engine::UI::UIDocumentReference>()); return true; }),
        Canvas("renderModeValue", NLS_TYPEOF(uint32_t),
            [](CanvasType& value) { return meta::Variant(value.GetRenderModeValue()); },
            [](CanvasType& value, const meta::Variant& next) { if (!IsVariant<uint32_t>(next)) return false; value.SetRenderModeValue(next.GetValue<uint32_t>()); return true; }),
        Canvas("worldRenderPathValue", NLS_TYPEOF(uint32_t),
            [](CanvasType& value) { return meta::Variant(value.GetWorldRenderPathValue()); },
            [](CanvasType& value, const meta::Variant& next) { if (!IsVariant<uint32_t>(next)) return false; value.SetWorldRenderPathValue(next.GetValue<uint32_t>()); return true; }),
        Canvas("sortOrderScript", NLS_TYPEOF(int32_t),
            [](CanvasType& value) { return meta::Variant(value.GetSortOrderScript()); },
            [](CanvasType& value, const meta::Variant& next) { if (!IsVariant<int32_t>(next)) return false; value.SetSortOrderScript(next.GetValue<int32_t>()); return true; }),
        Canvas("sortLayer", NLS_TYPEOF(int32_t),
            [](CanvasType& value) { return meta::Variant(value.GetSortLayer()); },
            [](CanvasType& value, const meta::Variant& next) { if (!IsVariant<int32_t>(next)) return false; value.SetSortLayer(next.GetValue<int32_t>()); return true; }),
        Canvas("cameraId", NLS_TYPEOF(uint64_t),
            [](CanvasType& value) { return meta::Variant(value.GetCameraId()); },
            [](CanvasType& value, const meta::Variant& next) { if (!IsVariant<uint64_t>(next)) return false; value.SetCameraId(next.GetValue<uint64_t>()); return true; }),
        Canvas("updatePolicy", NLS_TYPEOF(uint32_t),
            [](CanvasType& value) { return meta::Variant(value.GetUpdatePolicyValue()); },
            [](CanvasType& value, const meta::Variant& next) { if (!IsVariant<uint32_t>(next)) return false; value.SetUpdatePolicyValue(next.GetValue<uint32_t>()); return true; }),
        Canvas("dpiScaleMode", NLS_TYPEOF(uint32_t),
            [](CanvasType& value) { return meta::Variant(value.GetDpiScaleModeValue()); },
            [](CanvasType& value, const meta::Variant& next) { if (!IsVariant<uint32_t>(next)) return false; value.SetDpiScaleModeValue(next.GetValue<uint32_t>()); return true; }),
        Canvas("referenceResolution", NLS_TYPEOF(Maths::Vector2),
            [](CanvasType& value) { return meta::Variant(value.GetReferenceResolutionInspector()); },
            [](CanvasType& value, const meta::Variant& next) { if (!IsVariant<Maths::Vector2>(next)) return false; value.SetReferenceResolutionInspector(next.GetValue<Maths::Vector2>()); return true; }),
        Canvas("dpiMatch", NLS_TYPEOF(float),
            [](CanvasType& value) { return meta::Variant(value.GetDpiMatch()); },
            [](CanvasType& value, const meta::Variant& next) { if (!IsVariant<float>(next)) return false; value.SetDpiMatch(next.GetValue<float>()); return true; }),
        Canvas("worldSize", NLS_TYPEOF(Maths::Vector2),
            [](CanvasType& value) { return meta::Variant(value.GetWorldSizeInspector()); },
            [](CanvasType& value, const meta::Variant& next) { if (!IsVariant<Maths::Vector2>(next)) return false; value.SetWorldSizeInspector(next.GetValue<Maths::Vector2>()); return true; }),
        Canvas("pixelsPerUnit", NLS_TYPEOF(float),
            [](CanvasType& value) { return meta::Variant(value.GetPixelsPerUnit()); },
            [](CanvasType& value, const meta::Variant& next) { if (!IsVariant<float>(next)) return false; value.SetPixelsPerUnit(next.GetValue<float>()); return true; }),
        Canvas("depthTest", NLS_TYPEOF(bool),
            [](CanvasType& value) { return meta::Variant(value.GetDepthTest()); },
            [](CanvasType& value, const meta::Variant& next) { if (!IsVariant<bool>(next)) return false; value.SetDepthTest(next.GetValue<bool>()); return true; }),
        Canvas("depthWrite", NLS_TYPEOF(bool),
            [](CanvasType& value) { return meta::Variant(value.GetDepthWrite()); },
            [](CanvasType& value, const meta::Variant& next) { if (!IsVariant<bool>(next)) return false; value.SetDepthWrite(next.GetValue<bool>()); return true; }),
        Canvas("alwaysOnTop", NLS_TYPEOF(bool),
            [](CanvasType& value) { return meta::Variant(value.GetAlwaysOnTop()); },
            [](CanvasType& value, const meta::Variant& next) { if (!IsVariant<bool>(next)) return false; value.SetAlwaysOnTop(next.GetValue<bool>()); return true; })};
}

meta::Type ScriptValueType(const NLS::Scripting::ScriptType& type)
{
    using NLS::Scripting::ScriptValueKind;
    switch (type.kind)
    {
    case ScriptValueKind::Bool: return NLS_TYPEOF(bool);
    case ScriptValueKind::Int8: return NLS_TYPEOF(int8_t);
    case ScriptValueKind::UInt8: return NLS_TYPEOF(uint8_t);
    case ScriptValueKind::Int16: return NLS_TYPEOF(int16_t);
    case ScriptValueKind::UInt16: return NLS_TYPEOF(uint16_t);
    case ScriptValueKind::Int32: return NLS_TYPEOF(int32_t);
    case ScriptValueKind::UInt32: return NLS_TYPEOF(uint32_t);
    case ScriptValueKind::Int64:
    case ScriptValueKind::Enum: return NLS_TYPEOF(int64_t);
    case ScriptValueKind::UInt64: return NLS_TYPEOF(uint64_t);
    case ScriptValueKind::Float: return NLS_TYPEOF(float);
    case ScriptValueKind::Double: return NLS_TYPEOF(double);
    case ScriptValueKind::String: return NLS_TYPEOF(std::string);
    case ScriptValueKind::Vector2: return NLS_TYPEOF(Maths::Vector2);
    case ScriptValueKind::Vector3: return NLS_TYPEOF(Maths::Vector3);
    case ScriptValueKind::Vector4: return NLS_TYPEOF(Maths::Vector4);
    case ScriptValueKind::Quaternion: return NLS_TYPEOF(Maths::Quaternion);
    case ScriptValueKind::Color: return NLS_TYPEOF(Maths::Color);
    case ScriptValueKind::ObjectReference: return NLS_TYPEOF(Engine::Serialize::PPtr<Engine::GameObject>);
    case ScriptValueKind::Struct:
    case ScriptValueKind::Null:
    default: return {};
    }
}

std::optional<meta::Variant> ToVariant(const NLS::Scripting::ScriptValue& value, const NLS::Scripting::ScriptType& type)
{
    using NLS::Scripting::ScriptValueKind;
    switch (type.kind)
    {
    case ScriptValueKind::Bool: if (const auto* v = std::get_if<bool>(&value)) return meta::Variant(*v); break;
    case ScriptValueKind::Int8: if (const auto* v = std::get_if<int8_t>(&value)) return meta::Variant(*v); break;
    case ScriptValueKind::UInt8: if (const auto* v = std::get_if<uint8_t>(&value)) return meta::Variant(*v); break;
    case ScriptValueKind::Int16: if (const auto* v = std::get_if<int16_t>(&value)) return meta::Variant(*v); break;
    case ScriptValueKind::UInt16: if (const auto* v = std::get_if<uint16_t>(&value)) return meta::Variant(*v); break;
    case ScriptValueKind::Int32: if (const auto* v = std::get_if<int32_t>(&value)) return meta::Variant(*v); break;
    case ScriptValueKind::UInt32: if (const auto* v = std::get_if<uint32_t>(&value)) return meta::Variant(*v); break;
    case ScriptValueKind::Int64: if (const auto* v = std::get_if<int64_t>(&value)) return meta::Variant(*v); break;
    case ScriptValueKind::UInt64: if (const auto* v = std::get_if<uint64_t>(&value)) return meta::Variant(*v); break;
    case ScriptValueKind::Float: if (const auto* v = std::get_if<float>(&value)) return meta::Variant(*v); break;
    case ScriptValueKind::Double: if (const auto* v = std::get_if<double>(&value)) return meta::Variant(*v); break;
    case ScriptValueKind::String: if (const auto* v = std::get_if<std::string>(&value)) return meta::Variant(*v); break;
    case ScriptValueKind::Enum: if (const auto* v = std::get_if<NLS::Scripting::ScriptEnumValue>(&value)) return meta::Variant(v->value); break;
    case ScriptValueKind::Vector2: if (const auto* v = std::get_if<Maths::Vector2>(&value)) return meta::Variant(*v); break;
    case ScriptValueKind::Vector3: if (const auto* v = std::get_if<Maths::Vector3>(&value)) return meta::Variant(*v); break;
    case ScriptValueKind::Vector4: if (const auto* v = std::get_if<Maths::Vector4>(&value)) return meta::Variant(*v); break;
    case ScriptValueKind::Quaternion: if (const auto* v = std::get_if<Maths::Quaternion>(&value)) return meta::Variant(*v); break;
    case ScriptValueKind::Color: if (const auto* v = std::get_if<Maths::Color>(&value)) return meta::Variant(*v); break;
    case ScriptValueKind::ObjectReference:
        if (const auto* v = std::get_if<NLS::Scripting::NativeObjectHandle>(&value))
            return meta::Variant(Engine::Serialize::PPtr<Engine::GameObject>(v->GetInstanceId()));
        // Asset references are durable script payload, but this accessor
        // backend can only expose a resolved editor Object. Keep an unresolved
        // asset reference as null rather than converting it to a string and
        // corrupting the ObjectReference stream type.
        if (std::holds_alternative<NLS::Scripting::ScriptObjectReference>(value))
            return meta::Variant(Engine::Serialize::PPtr<Engine::GameObject> {});
        break;
    default: break;
    }
    return std::nullopt;
}

std::optional<NLS::Scripting::ScriptValue> ToScriptValue(const meta::Variant& value, const NLS::Scripting::ScriptType& type)
{
    if (!value.IsValid()) return std::nullopt;
    using NLS::Scripting::ScriptValueKind;
    switch (type.kind)
    {
    case ScriptValueKind::Bool: return value.GetValue<bool>();
    case ScriptValueKind::Int8: return value.GetValue<int8_t>();
    case ScriptValueKind::UInt8: return value.GetValue<uint8_t>();
    case ScriptValueKind::Int16: return value.GetValue<int16_t>();
    case ScriptValueKind::UInt16: return value.GetValue<uint16_t>();
    case ScriptValueKind::Int32: return value.GetValue<int32_t>();
    case ScriptValueKind::UInt32: return value.GetValue<uint32_t>();
    case ScriptValueKind::Int64: return value.GetValue<int64_t>();
    case ScriptValueKind::UInt64: return value.GetValue<uint64_t>();
    case ScriptValueKind::Float: return value.GetValue<float>();
    case ScriptValueKind::Double: return value.GetValue<double>();
    case ScriptValueKind::String: return value.GetValue<std::string>();
    case ScriptValueKind::Enum: return NLS::Scripting::ScriptEnumValue {type.id, value.GetValue<int64_t>()};
    case ScriptValueKind::Vector2: return value.GetValue<Maths::Vector2>();
    case ScriptValueKind::Vector3: return value.GetValue<Maths::Vector3>();
    case ScriptValueKind::Vector4: return value.GetValue<Maths::Vector4>();
    case ScriptValueKind::Quaternion: return value.GetValue<Maths::Quaternion>();
    case ScriptValueKind::Color: return value.GetValue<Maths::Color>();
    case ScriptValueKind::ObjectReference:
        return NLS::Scripting::NativeObjectHandle::FromInstanceId(
            value.GetValue<Engine::Serialize::PPtr<Engine::GameObject>>().GetInstanceID());
    default: return std::nullopt;
    }
}

std::optional<NLS::Scripting::ScriptValue> ReadScriptValue(
    const NLS::Scripting::ScriptComponent& component,
    const NLS::Scripting::ScriptFieldDescriptor& descriptor)
{
    NLS::Scripting::ScriptValue runtime;
    if (component.GetRuntimeSerializedField(descriptor.id, runtime)) return runtime;
    if (const auto* stored = component.FindSerializedField(descriptor.id)) return *stored;
    if (descriptor.defaultValue.has_value()) return *descriptor.defaultValue;
    using NLS::Scripting::ScriptValueKind;
    switch (descriptor.type.kind)
    {
    case ScriptValueKind::Bool: return false;
    case ScriptValueKind::Int8: return int8_t {};
    case ScriptValueKind::UInt8: return uint8_t {};
    case ScriptValueKind::Int16: return int16_t {};
    case ScriptValueKind::UInt16: return uint16_t {};
    case ScriptValueKind::Int32: return int32_t {};
    case ScriptValueKind::UInt32: return uint32_t {};
    case ScriptValueKind::Int64: return int64_t {};
    case ScriptValueKind::UInt64: return uint64_t {};
    case ScriptValueKind::Float: return 0.0f;
    case ScriptValueKind::Double: return 0.0;
    case ScriptValueKind::String: return std::string {};
    case ScriptValueKind::Enum: return NLS::Scripting::ScriptEnumValue {descriptor.type.id, 0};
    case ScriptValueKind::Vector2: return Maths::Vector2 {};
    case ScriptValueKind::Vector3: return Maths::Vector3 {};
    case ScriptValueKind::Vector4: return Maths::Vector4 {};
    case ScriptValueKind::Quaternion: return Maths::Quaternion {};
    case ScriptValueKind::Color: return Maths::Color {};
    default: return std::nullopt;
    }
}

std::vector<Descriptor> ScriptDescriptors(const NLS::Scripting::ScriptComponent& component)
{
    const auto* script = component.GetScriptTypeDescriptor();
    std::vector<Descriptor> result;
    Descriptor scriptAsset;
    scriptAsset.fieldId = StableFieldId("ScriptComponent", "scriptAsset");
    scriptAsset.name = "scriptAsset";
    scriptAsset.path = scriptAsset.name;
    scriptAsset.type = NLS_TYPEOF(std::string);
    scriptAsset.readOnly = true;
    scriptAsset.read = [](Object& object)
    {
        const auto* scriptComponent = dynamic_cast<const NLS::Scripting::ScriptComponent*>(&object);
        if (scriptComponent == nullptr)
            return meta::Variant {};
        const auto& asset = scriptComponent->GetScriptAsset();
        if (!asset.sourcePath.empty())
            return meta::Variant(asset.sourcePath);
        if (asset.assetId.IsValid())
            return meta::Variant("Missing Script (" + asset.assetId.ToString() + ")");
        return meta::Variant(std::string("Missing Script"));
    };
    result.push_back(std::move(scriptAsset));
    // The Script Asset row stays inspectable while an assembly reload has not
    // produced a descriptor yet. Field rows appear once the canonical script
    // schema is available; neither case falls back to a UI binding bridge.
    if (script == nullptr) return result;
    for (const auto& field : script->fields)
    {
        const auto type = ScriptValueType(field.type);
        if (!field.serialized || field.id == 0u || field.name.empty() || !type.IsValid()) continue;
        Descriptor descriptor;
        descriptor.name = field.name;
        descriptor.path = descriptor.name;
        descriptor.fieldId = field.id;
        descriptor.aliases = field.aliases;
        descriptor.type = type;
        descriptor.readOnly = field.metadata.readOnly;
        descriptor.read = [field](Object& object)
        {
            auto* scriptComponent = dynamic_cast<NLS::Scripting::ScriptComponent*>(&object);
            if (scriptComponent == nullptr) return meta::Variant {};
            const auto source = ReadScriptValue(*scriptComponent, field);
            const auto value = source.has_value() ? ToVariant(*source, field.type) : std::nullopt;
            return value.has_value() ? *value : meta::Variant {};
        };
        if (!descriptor.readOnly)
        {
            descriptor.write = [field](Object& object, const meta::Variant& value)
            {
                auto* scriptComponent = dynamic_cast<NLS::Scripting::ScriptComponent*>(&object);
                const auto next = ToScriptValue(value, field.type);
                return scriptComponent != nullptr && next.has_value() && scriptComponent->SetSerializedField(field.id, *next);
            };
        }
        result.push_back(std::move(descriptor));
    }
    return result;
}

bool IsSerializableScriptField(const NLS::Scripting::ScriptFieldDescriptor& field)
{
    // Keep this predicate deliberately identical to the script Inspector's
    // inclusion rule. A field omitted from this stream while still visible in
    // the legacy Inspector would silently disappear from a custom editor.
    return field.serialized && field.id != 0u && !field.name.empty() && field.type.IsValid();
}

std::vector<const NLS::Scripting::ScriptFieldDescriptor*> GetSerializableScriptFields(
    const NLS::Scripting::ScriptComponent& component)
{
    std::vector<const NLS::Scripting::ScriptFieldDescriptor*> result;
    const auto* script = component.GetScriptTypeDescriptor();
    if (script == nullptr)
        return result;
    result.reserve(script->fields.size());
    for (const auto& field : script->fields)
    {
        if (IsSerializableScriptField(field))
            result.push_back(&field);
    }
    return result;
}

bool HasMatchingScriptSchema(
    const NLS::Scripting::ScriptComponent& first,
    const NLS::Scripting::ScriptComponent& other)
{
    const auto firstFields = GetSerializableScriptFields(first);
    const auto otherFields = GetSerializableScriptFields(other);
    if (firstFields.size() != otherFields.size())
        return false;

    for (size_t index = 0u; index < firstFields.size(); ++index)
    {
        const auto& left = *firstFields[index];
        const auto& right = *otherFields[index];
        // The byte-stream layout is positional; an unequal script schema must
        // stay on the compatibility backend rather than use the first target's
        // layout for every selected component.
        if (left.id != right.id || left.name != right.name ||
            left.type.kind != right.type.kind || left.type.id != right.type.id ||
            !SameType(ScriptValueType(left.type), ScriptValueType(right.type)))
        {
            return false;
        }
    }
    return true;
}

std::vector<Descriptor> DescriptorsFor(Object& object, const bool componentHeaderOnly)
{
    if (componentHeaderOnly)
    {
        if (auto* component = dynamic_cast<Engine::Components::Component*>(&object))
        {
            auto descriptors = ComponentHeaderDescriptors();
            if (dynamic_cast<Engine::Components::TransformComponent*>(component) != nullptr)
                descriptors.front().readOnly = true;
            return descriptors;
        }
        return {};
    }
    if (dynamic_cast<Engine::GameObject*>(&object) != nullptr)
        return GameObjectDescriptors();
    if (dynamic_cast<Engine::Components::TransformComponent*>(&object) != nullptr)
        return TransformDescriptors();
    if (dynamic_cast<Engine::Components::UICanvasComponent*>(&object) != nullptr)
        return CanvasDescriptors();
    if (auto* script = dynamic_cast<NLS::Scripting::ScriptComponent*>(&object))
        return ScriptDescriptors(*script);
    return {};
}

const Descriptor* FindDescriptor(const std::vector<Descriptor>& descriptors, const std::string_view path)
{
    const auto name = RootName(path);
    const auto it = std::find_if(descriptors.begin(), descriptors.end(), [&path, &name](const Descriptor& descriptor)
    { return descriptor.path == path || descriptor.name == name; });
    return it == descriptors.end() ? nullptr : &*it;
}

bool AppendTree(
    SerializedTypeTree& destination,
    const SerializedTypeTree& source,
    const Descriptor& descriptor,
    const Engine::Serialize::SerializedNodeId parentId)
{
    const auto* root = source.FindNode(source.GetRoot());
    if (root == nullptr) return false;
    std::unordered_map<Engine::Serialize::SerializedNodeId, Engine::Serialize::SerializedNodeId> remap;
    for (const auto& sourceNode : source.GetNodes())
    {
        SerializedTypeTreeNode node = sourceNode;
        if (sourceNode.id == root->id)
        {
            node.parent = parentId;
            node.serializedName = descriptor.name;
            node.fieldId = descriptor.fieldId;
            node.aliases = descriptor.aliases;
            if (descriptor.readOnly)
                node.flags = node.flags | Engine::Serialize::SerializedNodeFlags::ReadOnly;
        }
        else
        {
            const auto parent = remap.find(sourceNode.parent);
            if (parent == remap.end()) return false;
            node.parent = parent->second;
        }
        node.id = node.firstChild = node.nextSibling = 0u;
        remap.emplace(sourceNode.id, destination.AddNode(std::move(node)));
    }
    return true;
}

std::shared_ptr<SerializedTypeTree> BuildTree(const std::vector<Descriptor>& descriptors)
{
    auto tree = std::make_shared<SerializedTypeTree>();
    SerializedTypeTreeNode root;
    root.serializedName = "";
    root.typeName = "AccessorSerializedObject";
    root.kind = Engine::Serialize::SerializedNodeKind::Generic;
    tree->AddNode(std::move(root));
    std::unordered_map<std::string, Engine::Serialize::SerializedNodeId> containers;
    for (const auto& descriptor : descriptors)
    {
        const auto fieldTree = NativeSerializedStreamCodec::BuildTypeTree(descriptor.type, {"value", false});
        auto parentId = tree->GetRoot();
        const auto separator = descriptor.path.rfind('.');
        if (separator != std::string::npos)
        {
            const std::string containerName = descriptor.path.substr(0u, separator);
            const auto found = containers.find(containerName);
            if (found != containers.end())
            {
                parentId = found->second;
            }
            else
            {
                SerializedTypeTreeNode container;
                container.parent = tree->GetRoot();
                container.serializedName = containerName;
                container.typeName = containerName;
                container.kind = Engine::Serialize::SerializedNodeKind::Generic;
                parentId = tree->AddNode(std::move(container));
                containers.emplace(containerName, parentId);
            }
        }
        if (fieldTree == nullptr || !AppendTree(*tree, *fieldTree, descriptor, parentId)) return nullptr;
    }
    tree->RebuildIndexes();
    return tree;
}

std::optional<meta::Variant> Decode(const SerializedByteStream& stream, const Descriptor& descriptor, std::string& error)
{
    const auto cursor = NativeSerializedStreamCodec::ResolveCursor(
        std::make_shared<SerializedByteStream>(stream), descriptor.path);
    const auto tree = NativeSerializedStreamCodec::BuildTypeTree(descriptor.type, {"value", false});
    if (!cursor.has_value() || tree == nullptr) return std::nullopt;
    std::vector<uint8_t> bytes(cursor->GetByteSize());
    if (!cursor->Read(bytes.data(), bytes.size())) return std::nullopt;
    const auto& refs = stream.GetReferences() != nullptr ? *stream.GetReferences() : SerializedReferenceTable {};
    return NativeSerializedStreamCodec::DecodeVariant(
        bytes, *tree, descriptor.type, refs, &error, cursor->GetByteOffset());
}
} // namespace

std::shared_ptr<AccessorSerializedBackend> AccessorSerializedBackend::Create()
{
    return std::shared_ptr<AccessorSerializedBackend>(new AccessorSerializedBackend(false));
}

std::shared_ptr<AccessorSerializedBackend> AccessorSerializedBackend::CreateComponentHeader()
{
    return std::shared_ptr<AccessorSerializedBackend>(new AccessorSerializedBackend(true));
}

bool AccessorSerializedBackend::Supports(const SerializedTargetSet& targets)
{
    if (targets.Empty())
        return false;

    const auto* first = Resolve(targets.targets.front());
    if (dynamic_cast<const Engine::GameObject*>(first) != nullptr)
    {
        return std::all_of(targets.targets.begin(), targets.targets.end(), [](const auto& target)
        {
            return dynamic_cast<const Engine::GameObject*>(Resolve(target)) != nullptr;
        });
    }
    if (dynamic_cast<const Engine::Components::UICanvasComponent*>(first) != nullptr)
    {
        return std::all_of(targets.targets.begin(), targets.targets.end(), [](const auto& target)
        {
            return dynamic_cast<const Engine::Components::UICanvasComponent*>(Resolve(target)) != nullptr;
        });
    }
    if (dynamic_cast<const Engine::Components::TransformComponent*>(first) != nullptr)
    {
        return std::all_of(targets.targets.begin(), targets.targets.end(), [](const auto& target)
        {
            return dynamic_cast<const Engine::Components::TransformComponent*>(Resolve(target)) != nullptr;
        });
    }

    const auto* firstScript = dynamic_cast<const NLS::Scripting::ScriptComponent*>(first);
    if (firstScript == nullptr)
        return false;

    return std::all_of(targets.targets.begin(), targets.targets.end(), [firstScript](const auto& target)
    {
        const auto* script = dynamic_cast<const NLS::Scripting::ScriptComponent*>(Resolve(target));
        return script != nullptr && HasMatchingScriptSchema(*firstScript, *script);
    });
}

bool AccessorSerializedBackend::SupportsComponentHeader(const SerializedTargetSet& targets)
{
    if (targets.Empty())
        return false;

    const auto* first = dynamic_cast<const Engine::Components::Component*>(Resolve(targets.targets.front()));
    if (first == nullptr)
        return false;
    const auto firstType = first->GetType();
    return std::all_of(targets.targets.begin(), targets.targets.end(), [&firstType](const auto& target)
    {
        const auto* component = dynamic_cast<const Engine::Components::Component*>(Resolve(target));
        return component != nullptr && SameType(component->GetType(), firstType);
    });
}

Engine::Serialize::TypeTreeResult AccessorSerializedBackend::BuildTypeTree(
    const SerializedTargetSet& targets, const SerializedInspectorMode) const
{
    if (!(m_componentHeaderOnly ? SupportsComponentHeader(targets) : Supports(targets)))
        return {nullptr, "No accessor serializer is registered for this target set."};
    auto* object = Resolve(targets.targets.front());
    auto tree = object == nullptr ? nullptr : BuildTree(DescriptorsFor(*object, m_componentHeaderOnly));
    return tree == nullptr ? Engine::Serialize::TypeTreeResult {nullptr, "Failed to build accessor TypeTree."}
                           : Engine::Serialize::TypeTreeResult {std::move(tree), {}};
}

Engine::Serialize::StreamResult AccessorSerializedBackend::SerializeTarget(const SerializedTargetHandle& target,
    std::shared_ptr<const SerializedTypeTree> schema, SerializedReferenceTable& references) const
{
    auto* object = Resolve(target);
    if (object == nullptr || schema == nullptr) return {nullptr, "Accessor serialized target is no longer valid."};
    std::vector<uint8_t> bytes;
    std::string error;
    for (const auto& descriptor : DescriptorsFor(*object, m_componentHeaderOnly))
    {
        const auto fieldTree = NativeSerializedStreamCodec::BuildTypeTree(descriptor.type, {"value", false});
        const auto value = descriptor.read ? descriptor.read(*object) : meta::Variant {};
        std::vector<uint8_t> fieldBytes;
        if (fieldTree == nullptr || !value.IsValid() || !NativeSerializedStreamCodec::EncodeVariant(
                value, *fieldTree, references, fieldBytes, &error, bytes.size()))
            return {nullptr, error.empty() ? "Failed to encode accessor property: " + descriptor.name : std::move(error)};
        bytes.insert(bytes.end(), fieldBytes.begin(), fieldBytes.end());
    }
    auto stream = std::make_shared<SerializedByteStream>(std::move(schema));
    stream->SetBytes(std::move(bytes));
    stream->SetReferences(std::make_shared<SerializedReferenceTable>(references));
    return {std::move(stream), {}};
}

ValidationResult AccessorSerializedBackend::ValidateAndNormalize(const SerializedTargetHandle& target,
    const std::span<const SerializedPropertyPath> paths, SerializedByteStream& stream) const
{
    auto* object = Resolve(target);
    if (object == nullptr || stream.GetSchema() == nullptr)
        return ValidationResult::Failure("Accessor serialized target is no longer valid.");
    const auto descriptors = DescriptorsFor(*object, m_componentHeaderOnly);
    for (const auto& path : paths)
    {
        const auto* descriptor = FindDescriptor(descriptors, path.ToString());
        const auto cursor = NativeSerializedStreamCodec::ResolveCursor(
            std::make_shared<SerializedByteStream>(stream), path.ToString());
        if (descriptor == nullptr || descriptor->readOnly || !cursor.has_value())
            return ValidationResult::Failure("Accessor property is no longer writable: " + path.ToString());
    }
    return ValidationResult::Success();
}

ApplyResult AccessorSerializedBackend::DeserializeTarget(const SerializedTargetHandle& target,
    const SerializedByteStream& stream, const Undo::UndoExecutionContext&) const
{
    auto* object = Resolve(target);
    if (object == nullptr || stream.GetSchema() == nullptr)
        return ApplyResult::Failure("Accessor serialized target is no longer valid.");
    struct Change { const Descriptor* descriptor; meta::Variant before; meta::Variant after; };
    std::vector<Change> changes;
    std::string error;
    const auto descriptors = DescriptorsFor(*object, m_componentHeaderOnly);
    for (const auto& descriptor : descriptors)
    {
        if (descriptor.readOnly)
            continue;
        const auto after = Decode(stream, descriptor, error);
        const auto before = descriptor.read ? descriptor.read(*object) : meta::Variant {};
        if (!after.has_value() || !after->IsValid() || !before.IsValid())
            return ApplyResult::Failure(error.empty() ? "Failed to decode accessor property: " + descriptor.name : error);
        changes.push_back({&descriptor, before, *after});
    }
    for (size_t index = 0u; index < changes.size(); ++index)
    {
        const auto& change = changes[index];
        if (!change.descriptor->write || !change.descriptor->write(*object, change.after))
        {
            for (size_t rollback = index; rollback > 0u; --rollback)
            {
                const auto& previous = changes[rollback - 1u];
                static_cast<void>(previous.descriptor->write(*object, previous.before));
            }
            return ApplyResult::Failure("Accessor property rejected decoded value: " + change.descriptor->name);
        }
    }
    return ApplyResult::Success();
}

SerializedVersionStamp AccessorSerializedBackend::GetVersion(const SerializedTargetHandle& target) const
{
    const auto* object = Resolve(target);
    if (object == nullptr) return {};
    uint64_t schemaHash = static_cast<uint64_t>(std::hash<std::string> {}(object->GetType().GetName()));
    const auto combine = [](const uint64_t seed, const uint64_t value)
    {
        return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6u) + (seed >> 2u));
    };
    if (const auto* script = dynamic_cast<const NLS::Scripting::ScriptComponent*>(object))
    {
        const auto& asset = script->GetScriptAsset();
        schemaHash = combine(schemaHash, asset.scriptType);
        schemaHash = combine(schemaHash, asset.contentHash);
        schemaHash = combine(schemaHash, std::hash<std::string> {}(asset.sourcePath));
        schemaHash = combine(schemaHash, static_cast<uint64_t>(asset.language));
        if (const auto* descriptor = script->GetScriptTypeDescriptor())
        {
            schemaHash = combine(schemaHash, descriptor->id);
            schemaHash = combine(schemaHash, std::hash<std::string> {}(descriptor->name));
            for (const auto& field : descriptor->fields)
            {
                schemaHash = combine(schemaHash, field.id);
                schemaHash = combine(schemaHash, std::hash<std::string> {}(field.name));
                schemaHash = combine(schemaHash, field.type.id);
                schemaHash = combine(schemaHash, static_cast<uint64_t>(field.type.kind));
                schemaHash = combine(schemaHash, field.serialized ? 1u : 0u);
            }
        }
    }
    const auto instance = static_cast<uint64_t>(object->GetInstanceID());
    return {instance ^ (schemaHash << 1u), schemaHash, schemaHash};
}
} // namespace NLS::Editor::Serialization
