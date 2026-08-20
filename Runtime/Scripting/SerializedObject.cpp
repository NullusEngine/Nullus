#include "SerializedObject.h"

#include "ScriptComponent.h"

#include <algorithm>
#include <type_traits>

namespace NLS::Scripting
{
SerializedObject::SerializedObject(ScriptComponent& target)
    : m_target(&target)
{
    Update();
}

void SerializedObject::Update()
{
    ++m_generation;
    if (m_generation == 0)
        ++m_generation;

    m_descriptor = m_target ? m_target->GetScriptTypeDescriptor() : nullptr;
    m_entries.clear();
    m_order.clear();
    m_currentProperties.clear();
    if (!m_target || !m_descriptor)
        return;

    const auto& fields = m_target->GetSerializedFields();
    for (const auto& descriptor : m_descriptor->fields)
    {
        if (!descriptor.serialized || descriptor.id == 0)
            continue;

        SnapshotEntry entry;
        entry.descriptor = descriptor;
        entry.defaultValue = descriptor.defaultValue.has_value()
            && IsValueCompatible(descriptor.type, *descriptor.defaultValue)
            ? *descriptor.defaultValue
            : MakeDefaultValue(descriptor);
        const auto found = fields.find(descriptor.id);
        entry.value = found != fields.end() && IsValueCompatible(descriptor.type, found->second)
            ? found->second
            : entry.defaultValue;

        // Match the serialized-object refresh semantics in Play mode: read
        // the live script instance for the Inspector snapshot, while leaving
        // the persistent override map untouched.
        if (m_target->GetInstance().IsValid())
        {
            ScriptValue runtimeValue;
            if (m_target->GetRuntimeSerializedField(descriptor.id, runtimeValue)
                && IsValueCompatible(descriptor.type, runtimeValue))
            {
                entry.value = std::move(runtimeValue);
            }
        }
        entry.originalValue = entry.value;
        m_order.push_back(descriptor.id);
        m_entries.emplace(descriptor.id, std::move(entry));
    }

    for (size_t index = 0; index < m_order.size(); ++index)
    {
        auto property = std::unique_ptr<SerializedProperty>(new SerializedProperty(this, m_order[index], m_generation, index));
        auto* pointer = property.get();
        m_propertyStorage.push_back(std::move(property));
        m_currentProperties.emplace(m_order[index], pointer);
    }
}

bool SerializedObject::ApplyModifiedProperties()
{
    if (!m_target || !m_descriptor)
        return false;

    // All values have already passed the type check in SetValue.  Applying in
    // descriptor order preserves deterministic field-write order for backends.
    for (const auto fieldId : m_order)
    {
        auto* entry = FindEntry(fieldId);
        if (!entry || AreValuesEqual(entry->value, entry->originalValue))
            continue;
        if (!m_target->SetSerializedField(fieldId, entry->value))
            return false;
    }

    for (const auto fieldId : m_order)
    {
        auto* entry = FindEntry(fieldId);
        if (entry)
            entry->originalValue = entry->value;
    }
    return true;
}

bool SerializedObject::HasModifiedProperties() const
{
    for (const auto fieldId : m_order)
    {
        const auto* entry = FindEntry(fieldId);
        if (entry && !AreValuesEqual(entry->value, entry->originalValue))
            return true;
    }
    return false;
}

SerializedProperty* SerializedObject::FindProperty(std::string_view path)
{
    if (path.empty())
        return nullptr;
    for (const auto fieldId : m_order)
    {
        const auto* entry = FindEntry(fieldId);
        if (entry && (entry->descriptor.name == path
                      || std::find(entry->descriptor.aliases.begin(), entry->descriptor.aliases.end(), path)
                          != entry->descriptor.aliases.end()))
            return FindProperty(fieldId);
    }
    return nullptr;
}

SerializedProperty* SerializedObject::FindProperty(ScriptFieldId fieldId)
{
    const auto found = m_currentProperties.find(fieldId);
    return found == m_currentProperties.end() ? nullptr : found->second;
}

SerializedProperty* SerializedObject::GetIterator()
{
    return m_order.empty() ? nullptr : FindProperty(m_order.front());
}

bool SerializedObject::IsCurrent(const SerializedProperty& property) const
{
    if (property.m_owner != this || property.m_generation != m_generation)
        return false;
    const auto found = m_currentProperties.find(property.m_fieldId);
    return found != m_currentProperties.end() && found->second == &property;
}

SerializedObject::SnapshotEntry* SerializedObject::FindEntry(ScriptFieldId fieldId)
{
    const auto found = m_entries.find(fieldId);
    return found == m_entries.end() ? nullptr : &found->second;
}

const SerializedObject::SnapshotEntry* SerializedObject::FindEntry(ScriptFieldId fieldId) const
{
    const auto found = m_entries.find(fieldId);
    return found == m_entries.end() ? nullptr : &found->second;
}

const ScriptValue& SerializedObject::GetPropertyValue(const SerializedProperty& property)
{
    if (!IsCurrent(property))
    {
        static const ScriptValue invalidValue{};
        return invalidValue;
    }

    auto* entry = FindEntry(property.m_fieldId);
    if (!entry)
    {
        static const ScriptValue invalidValue{};
        return invalidValue;
    }

    // A staged Inspector edit owns the snapshot until ApplyModifiedProperties.
    // Do not overwrite it with a concurrent script mutation while the user is
    // typing or dragging a value.
    SyncRuntimeValue(*entry);
    return entry->value;
}

void SerializedObject::SyncRuntimeValue(SnapshotEntry& entry)
{
    if (!m_target || !AreValuesEqual(entry.value, entry.originalValue))
    {
        return;
    }

    if (!m_target->GetInstance().IsValid())
    {
        // Play has ended (or the component has not entered Play yet).  Drop a
        // previously observed live value and expose the persistent override or
        // schema default again without modifying the component.
        const auto* serialized = m_target->FindSerializedField(entry.descriptor.id);
        const auto persistentValue = serialized != nullptr
            && IsValueCompatible(entry.descriptor.type, *serialized)
            ? *serialized
            : entry.defaultValue;
        entry.value = persistentValue;
        entry.originalValue = entry.value;
        return;
    }

    ScriptValue runtimeValue;
    if (!m_target->GetRuntimeSerializedField(entry.descriptor.id, runtimeValue)
        || !IsValueCompatible(entry.descriptor.type, runtimeValue))
    {
        return;
    }

    // Keep the live value in the editor snapshot only.  The serialized field
    // map remains untouched, so stopping Play cannot persist runtime changes.
    entry.value = std::move(runtimeValue);
    entry.originalValue = entry.value;
}

SerializedProperty* SerializedObject::GetNextProperty(const SerializedProperty& property)
{
    if (!IsCurrent(property) || property.m_index + 1 >= m_order.size())
        return nullptr;
    return FindProperty(m_order[property.m_index + 1]);
}

bool SerializedObject::SetSnapshotValue(const SerializedProperty& property, const ScriptValue& value)
{
    if (!IsCurrent(property))
        return false;
    auto* entry = FindEntry(property.m_fieldId);
    if (!entry || !IsValueCompatible(entry->descriptor.type, value))
        return false;
    entry->value = value;
    return true;
}

bool SerializedObject::AreValuesEqual(const ScriptValue& left, const ScriptValue& right)
{
    if (left.index() != right.index())
        return false;
    return std::visit([](const auto& first, const auto& second) -> bool
    {
        using First = std::decay_t<decltype(first)>;
        using Second = std::decay_t<decltype(second)>;
        if constexpr (!std::is_same_v<First, Second>)
            return false;
        else if constexpr (std::is_same_v<First, NLS::Maths::Color>)
            return first.r == second.r && first.g == second.g && first.b == second.b && first.a == second.a;
        else if constexpr (std::is_same_v<First, NLS::Maths::Vector2>)
            return first.x == second.x && first.y == second.y;
        else if constexpr (std::is_same_v<First, NLS::Maths::Vector3>)
            return first.x == second.x && first.y == second.y && first.z == second.z;
        else if constexpr (std::is_same_v<First, NLS::Maths::Vector4>)
            return first.x == second.x && first.y == second.y && first.z == second.z && first.w == second.w;
        else if constexpr (std::is_same_v<First, NLS::Maths::Quaternion>)
            return first.x == second.x && first.y == second.y && first.z == second.z && first.w == second.w;
        else
            return first == second;
    }, left, right);
}

bool SerializedObject::IsValueCompatible(const ScriptType& type, const ScriptValue& value)
{
    const auto kind = std::visit([](const auto& item) -> ScriptValueKind
    {
        using Value = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Value, std::monostate>) return ScriptValueKind::Null;
        if constexpr (std::is_same_v<Value, bool>) return ScriptValueKind::Bool;
        if constexpr (std::is_same_v<Value, int8_t>) return ScriptValueKind::Int8;
        if constexpr (std::is_same_v<Value, uint8_t>) return ScriptValueKind::UInt8;
        if constexpr (std::is_same_v<Value, int16_t>) return ScriptValueKind::Int16;
        if constexpr (std::is_same_v<Value, uint16_t>) return ScriptValueKind::UInt16;
        if constexpr (std::is_same_v<Value, int32_t>) return ScriptValueKind::Int32;
        if constexpr (std::is_same_v<Value, uint32_t>) return ScriptValueKind::UInt32;
        if constexpr (std::is_same_v<Value, int64_t>) return ScriptValueKind::Int64;
        if constexpr (std::is_same_v<Value, uint64_t>) return ScriptValueKind::UInt64;
        if constexpr (std::is_same_v<Value, float>) return ScriptValueKind::Float;
        if constexpr (std::is_same_v<Value, double>) return ScriptValueKind::Double;
        if constexpr (std::is_same_v<Value, std::string>) return ScriptValueKind::String;
        if constexpr (std::is_same_v<Value, ScriptEnumValue>) return ScriptValueKind::Enum;
        if constexpr (std::is_same_v<Value, NLS::Maths::Vector2>) return ScriptValueKind::Vector2;
        if constexpr (std::is_same_v<Value, NLS::Maths::Vector3>) return ScriptValueKind::Vector3;
        if constexpr (std::is_same_v<Value, NLS::Maths::Vector4>) return ScriptValueKind::Vector4;
        if constexpr (std::is_same_v<Value, NLS::Maths::Quaternion>) return ScriptValueKind::Quaternion;
        if constexpr (std::is_same_v<Value, NLS::Maths::Color>) return ScriptValueKind::Color;
        if constexpr (std::is_same_v<Value, NativeObjectHandle>) return ScriptValueKind::ObjectReference;
        if constexpr (std::is_same_v<Value, ScriptObjectReference>) return ScriptValueKind::ObjectReference;
        if constexpr (std::is_same_v<Value, ScriptStructValue>) return ScriptValueKind::Struct;
        return ScriptValueKind::Null;
    }, value);

    // NativeObjectHandle is process-local and must never enter a serialized
    // snapshot, even though it shares the ObjectReference value kind.
    if (std::holds_alternative<NativeObjectHandle>(value))
        return false;
    if (kind != type.kind)
        return false;
    if (const auto* enumValue = std::get_if<ScriptEnumValue>(&value))
        return type.id == 0 || enumValue->typeId == 0 || enumValue->typeId == type.id;
    if (const auto* structValue = std::get_if<ScriptStructValue>(&value))
        return type.id == 0 || structValue->typeId == 0 || structValue->typeId == type.id;
    return true;
}

ScriptValue SerializedObject::MakeDefaultValue(const ScriptFieldDescriptor& descriptor)
{
    switch (descriptor.type.kind)
    {
    case ScriptValueKind::Bool: return false;
    case ScriptValueKind::Int8: return int8_t{};
    case ScriptValueKind::UInt8: return uint8_t{};
    case ScriptValueKind::Int16: return int16_t{};
    case ScriptValueKind::UInt16: return uint16_t{};
    case ScriptValueKind::Int32: return int32_t{};
    case ScriptValueKind::UInt32: return uint32_t{};
    case ScriptValueKind::Int64: return int64_t{};
    case ScriptValueKind::UInt64: return uint64_t{};
    case ScriptValueKind::Float: return 0.0f;
    case ScriptValueKind::Double: return 0.0;
    case ScriptValueKind::String: return std::string{};
    case ScriptValueKind::Enum: return ScriptEnumValue{descriptor.type.id, 0};
    case ScriptValueKind::Vector2: return NLS::Maths::Vector2{};
    case ScriptValueKind::Vector3: return NLS::Maths::Vector3{};
    case ScriptValueKind::Vector4: return NLS::Maths::Vector4{};
    case ScriptValueKind::Quaternion: return NLS::Maths::Quaternion{};
    case ScriptValueKind::Color: return NLS::Maths::Color{};
    case ScriptValueKind::ObjectReference: return ScriptObjectReference{};
    case ScriptValueKind::Struct: return ScriptStructValue{descriptor.type.id, {}};
    case ScriptValueKind::Null:
    default: return std::monostate{};
    }
}
}
