#pragma once

#include "UI/Widgets/AWidget.h"

namespace NLS::UI::Widgets::Visual
{
	/**
	* Simple widget that display a separator
	*/
	class NLS_CORE_API Separator : public AWidget
	{
	protected:
		void _Draw_Impl() override;
	};
}