#include "Panels/EditorStatusBar.h"

#include <sstream>

#include <ServiceLocator.h>

#include "Core/Editor.h"

namespace NLS::Editor::Panels
{
namespace
{
constexpr float kStatusBarHeight = 24.0f;
}

EditorStatusBar::EditorStatusBar()
    : PanelViewportBar("EditorStatusBar", UI::PanelViewportBar::EAnchor::BOTTOM, kStatusBarHeight, false)
{
}

void EditorStatusBar::DrawBarContent()
{
    std::string fpsLabel = "FPS: --";

    if (NLS::Core::ServiceLocator::Contains<Editor::Core::Editor>())
    {
        const auto& editor = NLS_SERVICE(Editor::Core::Editor);
        const float frameRate = editor.GetCurrentFrameRate();
        if (frameRate > 0.0f)
        {
            std::ostringstream stream;
            stream.setf(std::ios::fixed);
            stream.precision(1);
            stream << "FPS: " << frameRate;
            fpsLabel = stream.str();
        }
    }

    ImGui::SetCursorPosY(4.0f);
    ImGui::TextUnformatted(fpsLabel.c_str());
}
}
