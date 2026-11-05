#include "EditorGraphInput.h"
#include <imgui.h>
#include <stdexcept>

namespace NLS::Editor::Shortcuts
{
namespace ed = ax::NodeEditor;
EditorGraphInput::EditorGraphInput(EditorShortcutService& service) : m_service(service)
{
    if (ed::IsKeyboardInputManaged()) throw std::logic_error("Node keyboard input already has an owner");
    m_service.RegisterContext({"node-graph", "Node Graph", 80, "focused-panel", [this] { return HasFocus(); }});
    m_service.RegisterContext({"node-graph-clipboard", "Node Graph Clipboard", 85, "focused-panel", [this] { return HasFocus() && m_clipboard; }});
    auto add = [&](const char* id, ed::KeyboardShortcut shortcut, bool clipboard = false)
    {
        ShortcutCommand command; command.id = std::string("node-graph.") + id;
        command.context = clipboard ? "node-graph-clipboard" : "node-graph"; command.category = "Node Graph"; command.displayName = id;
        command.availability = [this] { return HasFocus(); };
        command.invoke = [this, shortcut](const InvocationContext&) { return Request(shortcut); };
        m_service.RegisterCommand(std::move(command));
    };
    add("frame", ed::KeyboardShortcut::FrameSelection);
    add("frame-and-zoom", ed::KeyboardShortcut::FrameSelectionAndZoom);
    add("delete", ed::KeyboardShortcut::Delete);
    add("copy", ed::KeyboardShortcut::Copy, true);
    add("cut", ed::KeyboardShortcut::Cut, true);
    add("paste", ed::KeyboardShortcut::Paste, true);
    add("duplicate", ed::KeyboardShortcut::Duplicate, true);
    add("create", ed::KeyboardShortcut::CreateNode, true);
    // Current graph clients own clipboard business operations at document scope.
    // They do not use BeginShortcut; their existing CommandIds remain authoritative.
    ed::SetKeyboardInputObserver(Observe, this);
}
EditorGraphInput::~EditorGraphInput()
{
    ed::SetKeyboardInputObserver(nullptr);
    for (const char* id : {"node-graph.frame", "node-graph.frame-and-zoom", "node-graph.delete", "node-graph.copy", "node-graph.cut", "node-graph.paste", "node-graph.duplicate", "node-graph.create"}) m_service.GetRegistry().UnregisterCommand(id);
}
bool EditorGraphInput::HasFocus() const
{
    return m_target && ImGui::GetCurrentContext() && m_frame == ImGui::GetFrameCount();
}
void EditorGraphInput::Observe(ed::EditorContext* context, bool focused, bool destroyed, bool clipboard, void* user)
{
    auto& self = *static_cast<EditorGraphInput*>(user);
    if (self.m_target == context && (!focused || destroyed)) self.m_target = nullptr;
    if (focused && !destroyed) { self.m_target = context; self.m_frame = ImGui::GetFrameCount(); self.m_clipboard = clipboard; }
}
CommandResult EditorGraphInput::Request(ed::KeyboardShortcut shortcut)
{
    if (!HasFocus()) return CommandResult::TargetExpired;
    auto* previous = ed::GetCurrentEditor(); ed::SetCurrentEditor(m_target);
    ed::RequestKeyboardShortcut(shortcut); ed::SetCurrentEditor(previous);
    return CommandResult::Completed;
}
}
