#include <algorithm>

#include "UI/Panels/APanel.h"

namespace NLS
{
	uint64_t UI::Panels::APanel::__PANEL_ID_INCREMENT = 0;

	UI::Panels::APanel::APanel()
	{
		m_panelID = "##" + std::to_string(__PANEL_ID_INCREMENT++);
	}

	void UI::Panels::APanel::Draw()
	{
		if (enabled)
			_Draw_Impl();
	}

	const std::string& UI::Panels::APanel::GetPanelID() const
	{
		return m_panelID;
	}
}

