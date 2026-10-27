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
	void RegisterProjectSettingsPanel(ProjectSettings& p_panel);
        void DrawDialogs();

    protected:
        void DrawBarContent() override;

    private:
        void DrawToolbarRow();
        void DrawSceneToolRow(float p_rowY, float p_availableWidth);
        void DrawSceneToolButton(
            const char* p_widgetId,
            Icons::IconId p_icon,
            const char* p_tooltip,
            Editor::Core::EGizmoOperation p_operation,
            bool p_active);
        bool DrawIconTextButton(
            const char* p_widgetId,
            Icons::IconId p_icon,
            const char* p_label,
            const char* p_tooltip);

    private:
        MenuBar m_menuBar;
        Toolbar m_toolbar;
        bool m_iconsPreloaded = false;
        bool m_idlePreloadComplete = false;
    };
}
