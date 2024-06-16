#pragma once

#include "UI/Widgets/AWidget.h"

namespace NLS::UI::Widgets::Layout
{
	/**
	* Widget that adds an empty line to the panel
	*/
	class NLS_CORE_API NewLine : public AWidget
	{
	protected:
		void _Draw_Impl() override;
	};
}