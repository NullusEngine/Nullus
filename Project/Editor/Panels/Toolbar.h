#pragma once

#include <cstdint>
#include <string>

#include <UI/Panels/PanelWindow.h>

#include "Core/EditorActions.h"

namespace NLS::Editor::Panels
{
	class Toolbar : public UI::PanelWindow
	{
	public:
		Toolbar(
			const std::string& p_title,
			bool p_opened,
			const UI::PanelWindowSettings& p_windowSettings
		);
		~Toolbar();

		void _Draw_Impl() override;

		void DrawToolbarContent();

	private:
		uint64_t m_editorModeChangedListener = 0;
		Editor::Core::EditorActions::EEditorMode m_mode = Editor::Core::EditorActions::EEditorMode::EDIT;
	};
}
