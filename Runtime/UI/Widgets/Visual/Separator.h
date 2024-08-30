#pragma once

#include "UI/Widgets/AWidget.h"

namespace NLS::UI::Widgets
{
	/**
	* Simple widget that display a separator
	*/
	class NLS_UI_API Separator : public AWidget
	{
	protected:
		void _Draw_Impl() override;
	};
}