#include "Panels/Toolbar.h"

#include "ImGui/imgui.h"

#include <ServiceLocator.h>

#include "Core/EditorActions.h"
#include "Core/EditorResources.h"
#include "Icons/Generated/IconIds.generated.h"
#include "Icons/IconPainter.h"
#include "UI/UIManager.h"

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
	using EEditorMode = Editor::Core::EditorActions::EEditorMode;

	bool playEnabled = true;
	bool pauseEnabled = false;
	bool stopEnabled = false;
	bool nextEnabled = false;
	switch (m_mode)
	{
	case EEditorMode::EDIT:
		break;
	case EEditorMode::PLAY:
		playEnabled = false;
		pauseEnabled = true;
		stopEnabled = true;
		nextEnabled = true;
		break;
	case EEditorMode::PAUSE:
	case EEditorMode::FRAME_BY_FRAME:
		pauseEnabled = false;
		stopEnabled = true;
		nextEnabled = true;
		break;
	}

	auto& painter = EDITOR_CONTEXT(editorResources)->GetIconPainter();
	auto draw = [&painter](const char* p_widgetId, Icons::IconId p_icon, const char* p_tooltip, const bool p_enabled)
	{
		Icons::IconButtonOptions options;
		options.tooltip = p_tooltip;
		options.enabled = p_enabled;
		return painter.Button(p_widgetId, p_icon, Icons::IconToken::Small, options);
	};

	if (draw("##ToolbarPlay", Icons::EditorIcons::ToolPlay, "Play", playEnabled))
		EDITOR_EXEC(StartPlaying());
	ImGui::SameLine(0.0f, 2.0f);
	if (draw("##ToolbarPause", Icons::EditorIcons::ToolPause, "Pause", pauseEnabled))
		EDITOR_EXEC(PauseGame());
	ImGui::SameLine(0.0f, 2.0f);
	if (draw("##ToolbarStop", Icons::EditorIcons::ToolStop, "Stop", stopEnabled))
		EDITOR_EXEC(StopPlaying());
	ImGui::SameLine(0.0f, 2.0f);
	if (draw("##ToolbarNext", Icons::EditorIcons::ToolStepForward, "Step Forward", nextEnabled))
		EDITOR_EXEC(NextFrame());
	ImGui::SameLine(0.0f, 12.0f);
	draw("##ToolbarRefresh", Icons::EditorIcons::ToolRefresh, "Refresh", true);
}
