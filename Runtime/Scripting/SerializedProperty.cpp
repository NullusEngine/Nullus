#include "SerializedProperty.h"

#include "SerializedObject.h"

namespace NLS::Scripting
{
namespace
{
const ScriptValue& InvalidValue()
{
    static const ScriptValue value{};
    return value;
}

const ScriptType& InvalidType()
{
    static const ScriptType type{};
    return type;
}
}

bool SerializedProperty::IsValid() const
{
    return m_owner != nullptr && m_owner->IsCurrent(*this);
}

std::string_view SerializedProperty::GetName() const
{
    if (!IsValid())
        return {};
    const auto* entry = m_owner->FindEntry(m_fieldId);
    return entry ? std::string_view(entry->descriptor.name) : std::string_view{};
}

const ScriptType& SerializedProperty::GetType() const
{
    if (!IsValid())
        return InvalidType();
    const auto* entry = m_owner->FindEntry(m_fieldId);
    return entry ? entry->descriptor.type : InvalidType();
}

const ScriptValue& SerializedProperty::GetValue() const
{
    return IsValid() ? m_owner->GetPropertyValue(*this) : InvalidValue();
}

bool SerializedProperty::SetValue(const ScriptValue& value)
{
    return IsValid() && m_owner->SetSnapshotValue(*this, value);
}

bool SerializedProperty::HasModifiedValue() const
{
    if (!IsValid())
        return false;
    const auto* entry = m_owner->FindEntry(m_fieldId);
    return entry && !m_owner->AreValuesEqual(entry->value, entry->originalValue);
}

bool SerializedProperty::ResetToDefault()
{
    if (!IsValid())
        return false;
    const auto* entry = m_owner->FindEntry(m_fieldId);
    return entry && SetValue(entry->defaultValue);
}

SerializedProperty* SerializedProperty::NextVisible(bool)
{
    return IsValid() ? m_owner->GetNextProperty(*this) : nullptr;
}
}
