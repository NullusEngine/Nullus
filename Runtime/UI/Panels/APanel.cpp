#include <algorithm>

#include "UI/Panels/APanel.h"

namespace NLS::UI
{
	uint64_t APanel::__PANEL_ID_INCREMENT = 0;

	APanel::APanel()
	{
		m_panelID = "##" + std::to_string(__PANEL_ID_INCREMENT++);
	}

	void APanel::Draw()
	{
		if (enabled)
			_Draw_Impl();
	}

	const std::string& APanel::GetPanelID() const
	{
		return m_panelID;
	}
}

