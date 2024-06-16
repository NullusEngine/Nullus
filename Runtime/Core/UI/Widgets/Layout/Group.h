#pragma once

#include <vector>

#include "UI/Internal/WidgetContainer.h"

namespace NLS::UI::Widgets::Layout
{
	/**
	* Widget that can contains other widgets
	*/
	class NLS_CORE_API Group : public AWidget, public Internal::WidgetContainer
	{
	protected:
		virtual void _Draw_Impl() override;
	};
}