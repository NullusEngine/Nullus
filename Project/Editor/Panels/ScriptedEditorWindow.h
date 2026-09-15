#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <UI/Panels/PanelWindow.h>

#include "Scripting/EditorScriptHost.h"

namespace NLS::Editor::Panels
{
// A dockable editor window whose content is drawn by a managed EditorWindow
// subclass.  The window owns one managed instance through the script ABI and
// recreates it lazily after a domain reload, so the panel itself - and the
// Window menu entry that points at it - stays valid for the session.
class ScriptedEditorWindow final : public UI::PanelWindow, public Scripting::IEditorScriptTarget
{
public:
    ScriptedEditorWindow(
        const std::string& p_title,
        std::string editorTypeName,
        const UI::PanelWindowSettings& p_windowSettings);
    ~ScriptedEditorWindow() override;

    // IEditorScriptTarget
    [[nodiscard]] bool IsInspectorTarget() const override { return false; }
    void ReportScriptError(std::string_view message) override;

    [[nodiscard]] const std::string& GetEditorTypeName() const { return m_editorTypeName; }
    [[nodiscard]] bool IsScriptDisabled() const { return m_disabled; }
    // Drops the managed instance so the next draw recreates it from the
    // reloaded editor-scripts assembly.
    void ResetManagedInstance() { ReleaseManagedInstance(); }

protected:
    void OnBeforeDrawWidgets() override;
    void OnAfterDrawWidgets() override;

private:
    void EnsureManagedInstance();
    void ReleaseManagedInstance();

    std::string m_editorTypeName;
    Scripting::EditorScriptHandle m_handle = 0u;
    int32_t m_failureCount = 0;
    bool m_disabled = false;
    std::string m_lastError;
};
} // namespace NLS::Editor::Panels
