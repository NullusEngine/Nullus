#pragma once

#include "ScriptTypes.h"

#include <cstddef>
#include <string_view>

namespace NLS::Scripting
{
class SerializedObject;

// A view over one entry in a SerializedObject snapshot.  The view does not
// own the target; its owner keeps it valid until the SerializedObject dies.
class NLS_SCRIPTING_API SerializedProperty final
{
public:
    bool IsValid() const;
    ScriptFieldId GetFieldId() const { return m_fieldId; }
    std::string_view GetName() const;
    std::string_view GetPropertyPath() const { return GetName(); }
    const ScriptType& GetType() const;

    const ScriptValue& GetValue() const;
    bool SetValue(const ScriptValue& value);
    bool HasModifiedValue() const;
    bool ResetToDefault();

    // Flat iterator support.  Nested properties are intentionally
    // not exposed until arrays/struct child paths are part of the schema.
    SerializedProperty* NextVisible(bool enterChildren = true);

private:
    friend class SerializedObject;

    SerializedProperty(
        SerializedObject* owner,
        ScriptFieldId fieldId,
        uint64_t generation,
        size_t index)
        : m_owner(owner), m_fieldId(fieldId), m_generation(generation), m_index(index)
    {
    }

    SerializedObject* m_owner = nullptr;
    ScriptFieldId m_fieldId = 0;
    uint64_t m_generation = 0;
    size_t m_index = 0;
};
}
