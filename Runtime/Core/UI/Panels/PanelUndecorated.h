#pragma once

#include "UI/Panels/APanelTransformable.h"

namespace NLS::UI::Panels
{
	/**
	* A simple panel that is transformable and without decorations (No background)
	*/
	class NLS_CORE_API PanelUndecorated : public APanelTransformable
	{
	public:
		void _Draw_Impl() override;

	private:
		int CollectFlags();
	};
}