#pragma once

#include <UI/Panels/PanelViewportBar.h>

#include "Core/GizmoOperation.h"
#include "Core/SceneViewImGuizmo.h"
#include "Panels/MenuBar.h"
#include "Panels/Toolbar.h"

namespace NLS::Editor::Panels
{
    inline const char* GetToolbarPivotIconId(const Editor::Core::SceneViewGizmoPivot p_pivot)
    {
        return p_pivot == Editor::Core::SceneViewGizmoPivot::Center
            ? "editor.icon.toolbar.center"
            : "editor.icon.toolbar.pivot";
    }

    class EditorTopBar : public UI::PanelViewportBar
    {
    public:
        EditorTopBar();

        void InitializeSettingsMenu();
        void HandleShortcuts(float p_deltaTime);
        void RegisterWindowPanel(const std::string& p_name, UI::PanelWindow& p_panel);
        void RegisterProjectSettingsPanel(ProjectSettings& p_panel);
        void DrawDialogs();

    protected:
        void DrawBarContent() override;

    private:
        void DrawToolbarRow();
        void DrawSceneToolRow(float p_rowY, float p_availableWidth);
        void DrawSceneToolButton(
            const char* p_iconId,
            const char* p_tooltip,
            Editor::Core::EGizmoOperation p_operation,
            bool p_active);
        bool DrawIconTextButton(const char* p_iconId, const char* p_label, const char* p_tooltip);

    private:
        MenuBar m_menuBar;
        Toolbar m_toolbar;
    };
}
