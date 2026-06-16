#include <UI/Widgets/Layout/Spacing.h>
#include "ImGui/imgui.h"
#include <UI/Widgets/Selection/ComboBox.h>

#include "Panels/Toolbar.h"
#include "Core/EditorActions.h"

#include <ServiceLocator.h>
#include "UI/Icons/FontAwesomeIconFont.h"
#include "UI/ImGuiExtensions/TimelineProfiler/IconsFontAwesome4.h"
#include "UI/UIManager.h"
using namespace NLS;
Editor::Panels::Toolbar::Toolbar
(
	const std::string& p_title,
	bool p_opened,
	const UI::PanelWindowSettings& p_windowSettings
) : PanelWindow(p_title, p_opened, p_windowSettings)
{
    (void)NLS::UI::Icons::EnsureFontAwesomeIconFontLoaded(14.0f);

	m_playButton	= &CreateWidget<UI::Widgets::Button>(ICON_FA_PLAY, Maths::Vector2{ 24, 22 });
	m_pauseButton	= &CreateWidget<UI::Widgets::Button>(ICON_FA_PAUSE, Maths::Vector2{ 24, 22 });
	m_stopButton	= &CreateWidget<UI::Widgets::Button>(ICON_FA_STOP, Maths::Vector2{ 24, 22 });
	m_nextButton	= &CreateWidget<UI::Widgets::Button>(ICON_FA_STEP_FORWARD, Maths::Vector2{ 24, 22 });

	CreateWidget<UI::Widgets::Spacing>(0).lineBreak = false;
	auto& refreshButton	= CreateWidget<UI::Widgets::Button>(ICON_FA_REFRESH, Maths::Vector2{ 24, 22 });

	auto styleToolbarButton = [](UI::Widgets::Button* p_button)
	{
		p_button->idleBackgroundColor = { 0.0f, 0.0f, 0.0f, 0.0f };
		p_button->hoveredBackgroundColor = { 0.24f, 0.24f, 0.24f, 1.0f };
		p_button->clickedBackgroundColor = { 0.18f, 0.18f, 0.18f, 1.0f };
		p_button->textColor = { 1.0f, 1.0f, 1.0f, 1.0f };
	};
	styleToolbarButton(m_playButton);
	styleToolbarButton(m_pauseButton);
	styleToolbarButton(m_stopButton);
	styleToolbarButton(m_nextButton);
	styleToolbarButton(&refreshButton);

	m_playButton->lineBreak		= false;
	m_pauseButton->lineBreak	= false;
	m_stopButton->lineBreak		= false;
	m_nextButton->lineBreak		= false;
	refreshButton.lineBreak		= false;

	m_playButton->ClickedEvent	+= EDITOR_BIND(StartPlaying);
	m_pauseButton->ClickedEvent	+= EDITOR_BIND(PauseGame);
	m_stopButton->ClickedEvent	+= EDITOR_BIND(StopPlaying);
	m_nextButton->ClickedEvent	+= EDITOR_BIND(NextFrame);

	m_editorModeChangedListener = EDITOR_EVENT(EditorModeChangedEvent) += [this](Editor::Core::EditorActions::EEditorMode p_newMode)
	{
		auto enable = [](UI::Widgets::Button* p_button, bool p_enable)
		{
			p_button->disabled = !p_enable;
			p_button->textColor = p_enable ? Maths::Color{1.0f, 1.0f, 1.0f, 1.0f} : Maths::Color{1.0f, 1.0f, 1.0f, 0.25f};
		};

		switch (p_newMode)
		{
		case Editor::Core::EditorActions::EEditorMode::EDIT:
			enable(m_playButton, true);
			enable(m_pauseButton, false);
			enable(m_stopButton, false);
			enable(m_nextButton, false);
			break;
		case Editor::Core::EditorActions::EEditorMode::PLAY:
			enable(m_playButton, false);
			enable(m_pauseButton, true);
			enable(m_stopButton, true);
			enable(m_nextButton, true);
			break;
		case Editor::Core::EditorActions::EEditorMode::PAUSE:
			enable(m_playButton, true);
			enable(m_pauseButton, false);
			enable(m_stopButton, true);
			enable(m_nextButton, true);
			break;
		case Editor::Core::EditorActions::EEditorMode::FRAME_BY_FRAME:
			enable(m_playButton, true);
			enable(m_pauseButton, false);
			enable(m_stopButton, true);
			enable(m_nextButton, true);
			break;
		}
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
    DrawWidgets();
}
