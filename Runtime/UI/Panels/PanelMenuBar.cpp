#include "UI/Panels/PanelMenuBar.h"

namespace NLS::UI
{
	void PanelMenuBar::_Draw_Impl()
	{
		if (!m_widgets.empty() && ImGui::BeginMainMenuBar())
		{
			DrawWidgets();
			ImGui::EndMainMenuBar();
		}
	}
}
