#pragma once

#include <vector>
#include <memory>
#include <algorithm>

#include <Eventing/Event.h>

#include "UI/Panels/APanel.h"
#include "UI/Widgets/Menu/MenuList.h"

namespace NLS::UI
{
	/**
	* A simple panel that will be displayed on the top side of the canvas
	*/
	class NLS_UI_API PanelMenuBar : public APanel
	{
	protected:
		void _Draw_Impl() override;
	};
}