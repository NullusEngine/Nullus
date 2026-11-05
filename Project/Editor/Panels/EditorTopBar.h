#pragma once

#include <UI/Panels/PanelViewportBar.h>

#include "Core/GizmoOperation.h"
#include "Core/SceneViewImGuizmo.h"
#include "Icons/Generated/IconIds.generated.h"
#include "Panels/MenuBar.h"
#include "Panels/Toolbar.h"

namespace NLS::Editor::Panels
{
    inline Icons::IconId GetToolbarPivotIconId(const Editor::Core::SceneViewGizmoPivot p_pivot)
    {
        return p_pivot == Editor::Core::SceneViewGizmoPivot::Center
            ? Icons::EditorIcons::ToolbarCenter
            : Icons::EditorIcons::ToolbarPivot;
    }

    class EditorTopBar : public UI::PanelViewportBar
    {
    public:
        EditorTopBar();

	void RegisterWindowPanel(const std::string& p_name, UI::PanelWindow& p_panel);
	// Adds a scripted editor command to the Window or Tools menu.
	bool RegisterCommand(const std::string& p_menuPath, std::function<void()> p_execute);
        void UnregisterCommand(const std::string& path);
	void RegisterProjectSettingsPanel(ProjectSettings& p_panel);
        void DrawDialogs();

    protected:
        void DrawBarContent() override;

    private:
        void DrawToolbarRow();

    private:
        MenuBar m_menuBar;
        Toolbar m_toolbar;
        bool m_iconsPreloaded = false;
        bool m_idlePreloadComplete = false;
    };
}
