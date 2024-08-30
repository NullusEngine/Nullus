#pragma once

#include <UI/Widgets/Buttons/ButtonImage.h>
#include <UI/Panels/PanelWindow.h>

namespace NLS::Editor::Panels
{
	class Toolbar : public UI::PanelWindow
	{
	public:
		/**
		* Constructor
		* @param p_title
		* @param p_opened
		* @param p_windowSettings
		*/
		Toolbar
		(
			const std::string& p_title,
			bool p_opened,
			const UI::PanelWindowSettings& p_windowSettings
		);

		/**
		* Custom implementation of the draw method
		*/
		void _Draw_Impl() override;

	private:
		UI::Widgets::ButtonImage* m_playButton;
		UI::Widgets::ButtonImage* m_pauseButton;
		UI::Widgets::ButtonImage* m_stopButton;
		UI::Widgets::ButtonImage* m_nextButton;
	};
}