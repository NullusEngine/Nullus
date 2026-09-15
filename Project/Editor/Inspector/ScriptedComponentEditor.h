#pragma once

#include <string>
#include <string_view>

#include "Inspector/ComponentEditor.h"
#include "Scripting/EditorScriptHost.h"

namespace NLS::Editor::Inspector
{
// Bridges one managed ComponentEditor-derived class into the native Inspector.
// The managed object never receives a raw component pointer: every value it
// reads or writes goes through SerializedObject/SerializedProperty, which is
// why PropertyField/DrawDefaultInspector are forwarded back into the same
// ComponentEditorContext the native editors use.
class ScriptedComponentEditor final : public ComponentEditor, public Scripting::IEditorScriptTarget
{
public:
    ScriptedComponentEditor(std::string editorTypeName, std::string targetTypeName);
    ~ScriptedComponentEditor() override;

    void OnEnable() override;
    void OnDisable() override;
    void OnInspectorGUI() override;

    // IEditorScriptTarget
    [[nodiscard]] bool IsInspectorTarget() const override { return true; }
    [[nodiscard]] const InspectorTargetSet* GetScriptTargets() const override;
    [[nodiscard]] std::string_view GetScriptSearchText() const override { return SearchText(); }
    [[nodiscard]] bool ScriptPropertyField(
        std::string_view propertyPath,
        const std::string& label,
        const std::string& tooltip,
        bool includeChildren) override;
    [[nodiscard]] bool ScriptDrawDefaultInspector() override;
    void ReportScriptError(std::string_view message) override;

    [[nodiscard]] const std::string& GetEditorTypeName() const { return m_editorTypeName; }
    [[nodiscard]] bool IsDisabledForSession() const { return m_disabled; }

private:
    void EnsureManagedInstance();
    void ReleaseManagedInstance();
    void DrawUnavailableMessage(std::string_view reason);

    std::string m_editorTypeName;
    std::string m_targetTypeName;
    Scripting::EditorScriptHandle m_handle = 0u;
    int32_t m_failureCount = 0;
    int32_t m_errorCount = 0;
    bool m_disabled = false;
    std::string m_lastError;
};
} // namespace NLS::Editor::Inspector
