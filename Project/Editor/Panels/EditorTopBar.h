#pragma once

#include <UI/Panels/PanelViewportBar.h>

#include "Core/GizmoOperation.h"
#include "Panels/MenuBar.h"
#include "Panels/Toolbar.h"

namespace NLS::Editor::Panels
{
    class EditorTopBar : public UI::PanelViewportBar
    {
    public:
        EditorTopBar();

        void InitializeSettingsMenu();
        void HandleShortcuts(float p_deltaTime);
        void RegisterWindowPanel(const std::string& p_name, UI::PanelWindow& p_panel);

    protected:
        void DrawBarContent() override;

    private:
        void DrawToolbarRow();
        void DrawSceneToolRow(float p_rowY, float p_availableWidth);
        void DrawSceneToolButton(const char* p_label, Editor::Core::EGizmoOperation p_operation, bool p_active);

    private:
        MenuBar m_menuBar;
        Toolbar m_toolbar;
    };
}
