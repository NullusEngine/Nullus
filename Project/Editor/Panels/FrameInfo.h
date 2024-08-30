#pragma once

#include <UI/Panels/PanelWindow.h>
#include <UI/Widgets/Texts/TextColored.h>
#include <UI/Widgets/Selection/CheckBox.h>
#include <UI/Widgets/Visual/Separator.h>

#include <Panels/AView.h>

#include <Rendering/Data/FrameInfo.h>

namespace NLS::Editor::Panels
{
	class FrameInfo : public UI::PanelWindow
	{
	public:
		/**
		* Constructor
		* @param p_title
		* @param p_opened
		* @param p_windowSettings
		*/
		FrameInfo(
			const std::string& p_title,
			bool p_opened,
			const UI::PanelWindowSettings& p_windowSettings
		);

		/**
		* Update frame info information
		* @param p_targetView
		*/
        void Update(AView* p_targetView);

	private:
		UI::Widgets::Text& m_viewNameText;
		UI::Widgets::Separator& m_separator;
		UI::Widgets::Text& m_batchCountText;
		UI::Widgets::Text& m_instanceCountText;
		UI::Widgets::Text& m_polyCountText;
		UI::Widgets::Text& m_vertexCountText;
	};
}