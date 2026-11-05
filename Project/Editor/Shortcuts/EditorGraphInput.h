#pragma once
#include "EditorShortcutService.h"
#include <imgui_node_editor.h>

namespace NLS::Editor::Shortcuts
{
// Adapts the node library's focused canvas to the existing Command Registry.
// The library keeps its document operations and pointer handling.
class EditorGraphInput final
{
public:
    explicit EditorGraphInput(EditorShortcutService& service);
    ~EditorGraphInput();
    bool HasFocus() const;
private:
    static void Observe(ax::NodeEditor::EditorContext*, bool focused, bool destroyed, bool clipboard, void* user);
    CommandResult Request(ax::NodeEditor::KeyboardShortcut shortcut);
    EditorShortcutService& m_service;
    ax::NodeEditor::EditorContext* m_target = nullptr;
    int m_frame = -1;
    bool m_clipboard = false;
};
}
