#pragma once

#include "UI/Widgets/AWidget.h"

namespace NLS::UI::Widgets::Layout
{
	/**
	* Widget that adds a space to the panel line
	*/
	class NLS_CORE_API Spacing : public AWidget
	{
	public:
		/**
		* Constructor
		* @param p_spaces
		*/
		Spacing(uint16_t p_spaces = 1);

	protected:
		void _Draw_Impl() override;

	public:
		uint16_t spaces = 1;
	};
}