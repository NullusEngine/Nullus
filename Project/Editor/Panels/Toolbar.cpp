#include "Panels/Toolbar.h"

#include "ImGui/imgui.h"

#include <ServiceLocator.h>

#include "Core/EditorActions.h"
#include "Core/EditorResources.h"
#include "Icons/Generated/IconIds.generated.h"
#include "Icons/IconPainter.h"
#include "UI/UIManager.h"
#include "Settings/EditorSettings.h"
#include "Shortcuts/EditorCommandInvocation.h"

using namespace NLS;

Editor::Panels::Toolbar::Toolbar
(
	const std::string& p_title,
	bool p_opened,
	const UI::PanelWindowSettings& p_windowSettings
) : PanelWindow(p_title, p_opened, p_windowSettings)
{
	m_editorModeChangedListener =
		EDITOR_EVENT(EditorModeChangedEvent) += [this](Editor::Core::EditorActions::EEditorMode p_newMode)
	{
		m_mode = p_newMode;
	};

	EDITOR_EXEC(SetEditorMode(Editor::Core::EditorActions::EEditorMode::EDIT));
}

Editor::Panels::Toolbar::~Toolbar()
{
	EDITOR_EVENT(EditorModeChangedEvent) -= m_editorModeChangedListener;
}

void Editor::Panels::Toolbar::_Draw_Impl()
{
	NLS_SERVICE(UI::UIManager).PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));

	UI::PanelWindow::_Draw_Impl();

	NLS_SERVICE(UI::UIManager).PopStyleVar();
}

void Editor::Panels::Toolbar::DrawToolbarContent()
{
    auto& controller = EDITOR_EXEC(GetPlayModeController());
    const auto state = controller.GetState();
    const bool busy = controller.IsTransitioning();
    const bool active = controller.IsActive();
    const bool paused = state == Editor::Core::PlaySessionState::Paused;
    const auto& tint=Settings::EditorSettings::GetGameViewSettingsObject().playTint;
    auto& painter = EDITOR_CONTEXT(editorResources)->GetIconPainter();
    auto& shortcuts = NLS_SERVICE(Shortcuts::EditorShortcutService);
    const auto playHint = std::string(active ? "Stop (" : "Play (") + shortcuts.GetBindingDisplayText("editor.play") + ")";
    const auto pauseHint = std::string(paused ? "Resume (" : "Pause (") + shortcuts.GetBindingDisplayText("editor.pause") + ")";
    const auto stepHint = "Step (" + shortcuts.GetBindingDisplayText("editor.step") + ")";
    auto draw = [&painter](const char* id, Icons::IconId icon, const char* tooltip, bool enabled)
    {
        Icons::IconButtonOptions options;
        options.tooltip = tooltip;
        options.enabled = enabled;
        return painter.Button(id, icon, Icons::IconToken::Small, options);
    };
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(tint.r,tint.g,tint.b,tint.a));
    if (draw("##ToolbarPlay", active ? Icons::EditorIcons::ToolStop : Icons::EditorIcons::ToolPlay,
        playHint.c_str(), !busy || active)) Shortcuts::InvokeEditorCommand("editor.play", Shortcuts::CommandSource::Toolbar);
    if (active) ImGui::PopStyleColor();
    ImGui::SameLine(0, 2);
    if (paused) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(tint.r,tint.g,tint.b,tint.a));
    if (draw("##ToolbarPause", Icons::EditorIcons::ToolPause, pauseHint.c_str(),
        active && !busy && state != Editor::Core::PlaySessionState::Faulted))
    {
        Shortcuts::InvokeEditorCommand("editor.pause", Shortcuts::CommandSource::Toolbar);
    }
    if (paused) ImGui::PopStyleColor();
    ImGui::SameLine(0, 2);
    if (draw("##ToolbarNext", Icons::EditorIcons::ToolStepForward, stepHint.c_str(),
        active && !busy && state != Editor::Core::PlaySessionState::Faulted)) Shortcuts::InvokeEditorCommand("editor.step", Shortcuts::CommandSource::Toolbar);
    if (state == Editor::Core::PlaySessionState::Faulted || !controller.GetError().empty())
    {
        ImGui::SameLine();
        ImGui::TextUnformatted(state == Editor::Core::PlaySessionState::Faulted ? "Play failed: Stop to retry recovery" : "Play entry failed");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", controller.GetError().c_str());
    }
}
