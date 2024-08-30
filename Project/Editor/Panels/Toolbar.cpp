#include <UI/Widgets/Layout/Spacing.h>
#include <UI/Widgets/Selection/ComboBox.h>

#include "Panels/Toolbar.h"
#include "Core/EditorActions.h"

#include <ServiceLocator.h>
#include <ResourceManagement/TextureManager.h>
#include "UI/UIManager.h"
using namespace NLS;
Editor::Panels::Toolbar::Toolbar
(
	const std::string& p_title,
	bool p_opened,
	const UI::PanelWindowSettings& p_windowSettings
) : PanelWindow(p_title, p_opened, p_windowSettings)
{
	std::string iconFolder = ":Textures/Icons/";

	auto& textureManager = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::TextureManager>();

	m_playButton	= &CreateWidget<UI::Widgets::ButtonImage>(EDITOR_CONTEXT(editorResources)->GetTexture("Button_Play")->GetTextureId(), Maths::Vector2{ 20, 20 });
	m_pauseButton	= &CreateWidget<UI::Widgets::ButtonImage>(EDITOR_CONTEXT(editorResources)->GetTexture("Button_Pause")->GetTextureId(), Maths::Vector2{ 20, 20 });
	m_stopButton	= &CreateWidget<UI::Widgets::ButtonImage>(EDITOR_CONTEXT(editorResources)->GetTexture("Button_Stop")->GetTextureId(), Maths::Vector2{ 20, 20 });
	m_nextButton	= &CreateWidget<UI::Widgets::ButtonImage>(EDITOR_CONTEXT(editorResources)->GetTexture("Button_Next")->GetTextureId(), Maths::Vector2{ 20, 20 });

	CreateWidget<UI::Widgets::Spacing>(0).lineBreak = false;
	auto& refreshButton	= CreateWidget<UI::Widgets::ButtonImage>(EDITOR_CONTEXT(editorResources)->GetTexture("Button_Refresh")->GetTextureId(), Maths::Vector2{ 20, 20 });

	m_playButton->lineBreak		= false;
	m_pauseButton->lineBreak	= false;
	m_stopButton->lineBreak		= false;
	m_nextButton->lineBreak		= false;
	refreshButton.lineBreak		= false;

	m_playButton->ClickedEvent	+= EDITOR_BIND(StartPlaying);
	m_pauseButton->ClickedEvent	+= EDITOR_BIND(PauseGame);
	m_stopButton->ClickedEvent	+= EDITOR_BIND(StopPlaying);
	m_nextButton->ClickedEvent	+= EDITOR_BIND(NextFrame);

	EDITOR_EVENT(EditorModeChangedEvent) += [this](Editor::Core::EditorActions::EEditorMode p_newMode)
	{
		auto enable = [](UI::Widgets::ButtonImage* p_button, bool p_enable)
		{
			p_button->disabled = !p_enable;
            p_button->tint = p_enable ? Maths::Color{1.0f, 1.0f, 1.0f, 1.0f} : Maths::Color{1.0f, 1.0f, 1.0f, 0.15f};
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

void Editor::Panels::Toolbar::_Draw_Impl()
{
    NLS_SERVICE(UI::UIManager).PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));

	UI::PanelWindow::_Draw_Impl();

	NLS_SERVICE(UI::UIManager).PopStyleVar();
}