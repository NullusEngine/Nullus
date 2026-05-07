#include "Panels/EditorStatusBar.h"

#include <sstream>

#include <imgui.h>
#include <Reflection/ReflectionDiagnostics.h>
#include <ServiceLocator.h>
#include <UI/UIManager.h>

#include "Core/Editor.h"

namespace NLS::Editor::Panels
{
namespace
{
constexpr float kStatusBarHeight = 24.0f;

float Scaled(const float p_value)
{
    return NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>()
        ? NLS_SERVICE(NLS::UI::UIManager).Scale(p_value)
        : p_value;
}

void OpenConsolePanel()
{
    if (!NLS::Core::ServiceLocator::Contains<Editor::Core::Editor>())
        return;

    NLS_SERVICE(Editor::Core::Editor).OpenConsole();
}

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

    ImGui::SetCursorPosY(Scaled(4.0f));
    ImGui::TextUnformatted(fpsLabel.c_str());

    const auto reflectionDiagnosticCounts = NLS::meta::ReflectionDiagnostics::Count();
    const auto reflectionWarningCount = reflectionDiagnosticCounts.warnings;
    const auto reflectionErrorCount = reflectionDiagnosticCounts.errors;
    if (reflectionWarningCount > 0 || reflectionErrorCount > 0)
    {
        std::ostringstream reflectionLabel;
        reflectionLabel << "Reflection: ";
        if (reflectionErrorCount > 0)
            reflectionLabel << reflectionErrorCount << " errors";
        if (reflectionErrorCount > 0 && reflectionWarningCount > 0)
            reflectionLabel << ", ";
        if (reflectionWarningCount > 0)
            reflectionLabel << reflectionWarningCount << " warnings";

        ImGui::SameLine(0.0f, Scaled(16.0f));
        const ImVec4 color = reflectionErrorCount > 0
            ? ImVec4(1.0f, 0.25f, 0.25f, 1.0f)
            : ImVec4(1.0f, 0.78f, 0.20f, 1.0f);
        ImGui::TextColored(color, "%s", reflectionLabel.str().c_str());
        if (ImGui::IsItemHovered())
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("Open Console");
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            OpenConsolePanel();
        }
    }
}
}
