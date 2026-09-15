#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace NLS::Editor::Panels
{
class ScriptedEditorWindow;
}

namespace NLS::Editor::Scripting
{
// Owns the runtime-created editor windows for the session.  Windows are keyed
// by their managed editor type name so a second Open() focuses the existing
// window instead of creating a duplicate, matching the external editor.
class EditorScriptWindowService final
{
public:
    static EditorScriptWindowService& Get();

    // Opens (or focuses) the window declared by the given managed editor type.
    void OpenWindow(std::string_view editorTypeName);
    // Closes the window but keeps the panel alive so the Window menu entry
    // stays valid; the managed instance is recreated on the next open.
    void CloseWindow(std::string_view editorTypeName);
    // Drops every managed instance while keeping the native panels registered.
    // Called after a domain reload so the next draw rebuilds them.
    void ReleaseManagedInstances();
    void CloseAllWindows();

    [[nodiscard]] std::size_t GetWindowCount() const { return m_windows.size(); }
    [[nodiscard]] bool HasScriptWindow(std::string_view editorTypeName) const;
    // Test/diagnostic access to the panel for a registered editor type.
    // Named "ScriptWindow" so it can never collide with the Win32 FindWindow
    // macro that <windows.h> defines.
    [[nodiscard]] Panels::ScriptedEditorWindow* FindScriptWindow(std::string_view editorTypeName) const;

private:
    EditorScriptWindowService() = default;

    std::unordered_map<std::string, Panels::ScriptedEditorWindow*> m_windows;
};
} // namespace NLS::Editor::Scripting
