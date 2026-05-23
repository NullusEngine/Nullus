#pragma once

#include <UI/Panels/PanelViewportBar.h>

namespace NLS::Editor::Panels
{
    class EditorStatusBar : public UI::PanelViewportBar
    {
    public:
        EditorStatusBar();

    protected:
        void DrawBarContent() override;

    };
}
