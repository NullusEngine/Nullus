#pragma once
#include "EditorShortcutContext.h"

namespace NLS::Editor::Shortcuts
{
struct EditorInputRoute
{
    bool windowFocused = true;
    bool textEditing = false;
    bool modal = false;
    bool menuActive = false;
    bool gameOwnsInput = false;
    bool allowEditorInGame = false;
    bool workbenchOwnsInput = false;
    std::string ownedScope;
    bool gestureOwnsEscape = false;
    bool graphOwnsInput = false;
    bool cameraOwnsInput = false;
    bool widgetOwnsPointer = false;
    bool widgetKeyboardActivation = false;
};

// Authorization is separate from command availability and from ImGui's native
// widget input. The caller provides ownership from the current UI frame.
class EditorInputRouter final
{
public:
    void SetRoute(EditorInputRoute route) { m_route = route; }
    const EditorInputRoute& GetRoute() const { return m_route; }
    bool AllowsOrdinaryCommands() const
    {
        return m_route.windowFocused && !m_route.textEditing && !m_route.modal && !m_route.menuActive && !m_route.widgetKeyboardActivation && !m_route.workbenchOwnsInput &&
            (!m_route.gameOwnsInput || m_route.allowEditorInGame);
    }
    bool AllowsControlCommands() const { return m_route.windowFocused && !m_route.modal; }
    bool AllowsScope(const std::string& scope) const
    {
        return m_route.windowFocused && !m_route.textEditing && !m_route.modal && !m_route.menuActive && !m_route.widgetKeyboardActivation &&
            (!m_route.gameOwnsInput || m_route.allowEditorInGame) &&
            (!m_route.workbenchOwnsInput || (!m_route.ownedScope.empty() && scope == m_route.ownedScope) ||
                (m_route.graphOwnsInput && (scope == "node-graph" || scope == "node-graph-clipboard")) || (m_route.cameraOwnsInput &&
                    (scope == "camera-angle" || scope == "camera-preview" || scope == ShortcutContexts::SceneViewFlyMode)));
    }
private:
    EditorInputRoute m_route;
};
}
