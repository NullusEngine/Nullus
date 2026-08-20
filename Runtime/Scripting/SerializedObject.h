#pragma once

#include "SerializedProperty.h"

#include <deque>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NLS::Scripting
{
class ScriptComponent;

// Editor-facing serialization snapshot.  Values are edited in the snapshot
// and are committed to the ScriptComponent only by ApplyModifiedProperties.
class NLS_SCRIPTING_API SerializedObject final
{
public:
    explicit SerializedObject(ScriptComponent& target);
    ~SerializedObject() = default;

    SerializedObject(const SerializedObject&) = delete;
    SerializedObject& operator=(const SerializedObject&) = delete;

    void Update();
    bool ApplyModifiedProperties();
    bool HasModifiedProperties() const;

    SerializedProperty* FindProperty(std::string_view path);
    SerializedProperty* FindProperty(ScriptFieldId fieldId);
    SerializedProperty* GetIterator();

    bool IsValid() const { return m_target != nullptr; }
    ScriptComponent* GetTarget() const { return m_target; }
    uint64_t GetVersion() const { return m_generation; }

private:
    friend class SerializedProperty;

    struct SnapshotEntry
    {
        ScriptFieldDescriptor descriptor;
        ScriptValue value;
        ScriptValue originalValue;
        ScriptValue defaultValue;
    };

    bool IsCurrent(const SerializedProperty& property) const;
    SnapshotEntry* FindEntry(ScriptFieldId fieldId);
    const SnapshotEntry* FindEntry(ScriptFieldId fieldId) const;
    const ScriptValue& GetPropertyValue(const SerializedProperty& property);
    void SyncRuntimeValue(SnapshotEntry& entry);
    SerializedProperty* GetNextProperty(const SerializedProperty& property);
    bool SetSnapshotValue(const SerializedProperty& property, const ScriptValue& value);
    static bool IsValueCompatible(const ScriptType& type, const ScriptValue& value);
    static bool AreValuesEqual(const ScriptValue& left, const ScriptValue& right);
    static ScriptValue MakeDefaultValue(const ScriptFieldDescriptor& descriptor);

    ScriptComponent* m_target = nullptr;
    const ScriptTypeDescriptor* m_descriptor = nullptr;
    uint64_t m_generation = 0;
    std::unordered_map<ScriptFieldId, SnapshotEntry> m_entries;
    std::vector<ScriptFieldId> m_order;
    std::unordered_map<ScriptFieldId, SerializedProperty*> m_currentProperties;
    // Keep prior property wrappers alive so callers can observe IsValid()==false
    // after Update(), rather than dereferencing a freed iterator object.
    std::deque<std::unique_ptr<SerializedProperty>> m_propertyStorage;
};
}
